#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/detail/block_reflector.hpp>
#include <crd/hesap/dense/detail/householder.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/multifrontal_qr.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/jobs/jobs.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace crd::hesap::direct
{
namespace
{
// Blocked-WY front factor (v5c-1d): panel width + the front-size gate. Large fronts (the dense
// near-root ones that stall the unblocked Householder at O(n³)) take the BLAS-3 compact-WY path;
// small fronts keep the proven unblocked path (no gemm-call overhead ⇒ no regression on the wins).
constexpr crd::u32 kQrPanelW = 48;   // compact-WY panel width (nb)
constexpr crd::u32 kQrBlockMin = 64; // block only fronts with ≥ this many rows

// Conjugate that is the IDENTITY for real T and crd::hesap::conj for Complex<R> — the v5c-2a real↔complex
// bridge (mirrors supernodal_cholesky's chol_conj). Lets the SAME reflector body compute A·P=Q·R (real,
// Qᵀ-apply) and A·P=Q·R with the Hermitian transpose (complex, Qᴴ-apply): the dot uses qr_conj(v)=vᴴ and
// the reflector scalar uses qr_conj(tau). For real every qr_conj is a no-op ⇒ the proven real path is
// bit-identical (the moat is unchanged).
template <typename T> [[nodiscard]] constexpr T qr_conj(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return conj(x);
    }
    else
    {
        return x;
    }
}
// Promote a real value to T (T{r,0} for Complex, r for real) — the R-diagonal store (beta is real for
// both make_householder and make_householder_complex).
template <typename T> [[nodiscard]] constexpr T qr_from_real(dense::RealType<T> r) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return T{r, dense::RealType<T>{0}};
    }
    else
    {
        return r;
    }
}
// Magnitude |x| as the real type — for the v5c-2b rank-revealing R-diagonal test.
template <typename T> [[nodiscard]] inline dense::RealType<T> qr_abs(const T& x) noexcept
{
    if constexpr (dense::is_complex_v<T>)
    {
        return std::sqrt(x.re * x.re + x.im * x.im);
    }
    else
    {
        return x < T{0} ? -x : x;
    }
}
} // namespace

sparse::SparsePattern ata_pattern(const sparse::SparsePattern& a, crd::memory::IAllocator* alloc)
{
    CRD_ASSERT_MSG(a.is_compressed(), "ata_pattern requires a compressed CSC pattern");
    CRD_ASSERT_MSG(a.format == sparse::SparseFormat::Csc, "ata_pattern requires CSC");

    const crd::u32 m = a.rows;
    const crd::u32 n = a.cols;
    sparse::SparsePattern out(alloc);
    out.rows = n;
    out.cols = n;
    out.format = sparse::SparseFormat::Csc;
    out.outer_ptr.resize(static_cast<crd::usize>(n) + 1); // value-init 0
    if (n == 0)
    {
        out.recompute_topology_hash();
        return out;
    }

    const crd::u32* ap = a.outer_ptr.data();
    const crd::u32* ai = a.inner_idx.data();

    // CSR of A (row -> columns), counting-sort over the row indices. arp[i+1] counts row i.
    crd::containers::Array<crd::u32> arp(alloc);
    arp.resize(static_cast<crd::usize>(m) + 1); // value-init 0
    for (crd::usize p = 0; p < a.inner_idx.size(); ++p)
    {
        ++arp[ai[p] + 1];
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        arp[i + 1] += arp[i];
    }
    crd::containers::Array<crd::u32> aci(alloc); // CSR column indices (ascending within a row by construction)
    aci.resize(a.inner_idx.size());
    crd::containers::Array<crd::u32> cur(alloc);
    cur.resize(static_cast<crd::usize>(m));
    for (crd::u32 i = 0; i < m; ++i)
    {
        cur[i] = arp[i];
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 p = ap[k]; p < ap[k + 1]; ++p)
        {
            const crd::u32 i = ai[p];
            aci[cur[i]++] = k;
        }
    }

    // AᵀA(:,k) = ∪_{i ∈ A(:,k)} CSR-row-i columns. Marker stamp = k (mark init n, an impossible column id).
    crd::containers::Array<crd::u32> mark(alloc);
    mark.resize(static_cast<crd::usize>(n));
    for (crd::u32 j = 0; j < n; ++j)
    {
        mark[j] = n;
    }
    crd::containers::Array<crd::u32> colbuf(alloc);
    colbuf.reserve(n);
    out.inner_idx.reserve(a.inner_idx.size() * 2 + n);
    for (crd::u32 k = 0; k < n; ++k)
    {
        out.outer_ptr[k] = static_cast<crd::u32>(out.inner_idx.size());
        colbuf.clear();
        for (crd::u32 p = ap[k]; p < ap[k + 1]; ++p)
        {
            const crd::u32 i = ai[p]; // a row of A in column k
            for (crd::u32 q = arp[i]; q < arp[i + 1]; ++q)
            {
                const crd::u32 j = aci[q]; // a column sharing row i with k
                if (mark[j] != k)
                {
                    mark[j] = k;
                    colbuf.push_back(j);
                }
            }
        }
        crd::containers::sort(colbuf.data(), colbuf.data() + colbuf.size());
        for (crd::u32 t = 0; t < colbuf.size(); ++t)
        {
            out.inner_idx.push_back(colbuf[t]);
        }
    }
    out.outer_ptr[n] = static_cast<crd::u32>(out.inner_idx.size());
    out.recompute_topology_hash();
    return out;
}

QrSymbolic multifrontal_qr_symbolic(const sparse::SparsePattern& a, crd::memory::IAllocator* alloc, crd::u32 nrelax)
{
    CRD_ASSERT_MSG(a.is_compressed(), "multifrontal_qr_symbolic requires a compressed CSC pattern");
    CRD_ASSERT_MSG(a.format == sparse::SparseFormat::Csc, "multifrontal_qr_symbolic requires CSC");

    QrSymbolic out(alloc);
    out.m = a.rows;
    out.n = a.cols;
    const crd::u32 m = a.rows;
    const crd::u32 n = a.cols;

    // 1. Front structure = chol(AᵀA) amalgamated supernodes. v5c-1e: the IMPLICIT symbolic — etree +
    // counts + per-supernode leading patterns (slead) of chol(AᵀA), computed WITHOUT forming AᵀA.
    // `symbolic_factorize_ata` is bit-for-bit identical to the prior explicit path
    // `symbolic_factorize(ata_pattern(a), …, /*supernodal_patterns=*/true)` (kept as the verifying
    // oracle in the tests), but skips the O(Σ nnz_row²) AᵀA clique-union that DOMINATED the symbolic
    // (measured: bcsstk13 ~12 ms → sub-ms; the explicit path's tax was 15–31% of the small-LS factor).
    ordering::SymbolicFactor sf = ordering::symbolic_factorize_ata(a, alloc);
    out.fronts = build_supernodal_symbolic(sf, alloc, nrelax);
    const crd::u32 nf = out.fronts.nsuper;

    out.sleft.resize(static_cast<crd::usize>(n) + 1); // value-init 0
    if (n == 0 || nf == 0)
    {
        return out;
    }

    // 2. Assembly tree: front f's parent = the front owning f's FIRST non-pivotal (contribution)
    // column. The front's column set fn = srow[srowp[f]..srowp[f+1]); the first nc are the pivots,
    // so srow[srowp[f] + nc] (when present) is the first contribution column ⇒ its owning front is
    // the parent. No contribution columns ⇒ a root.
    out.front_parent.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 nc = out.fronts.scol[f + 1] - out.fronts.scol[f];
        const crd::u32 rb = out.fronts.srowp[f];
        const crd::u32 rnum = out.fronts.srowp[f + 1] - rb;
        if (nc < rnum)
        {
            const crd::u32 first_off = out.fronts.srow[rb + nc];
            out.front_parent[f] = out.fronts.col_super[first_off];
        }
        else
        {
            out.front_parent[f] = ordering::kNoParent;
        }
    }
    out.front_post = ordering::postorder({out.front_parent.data(), out.front_parent.size()}, alloc);

    // 3. Row merge: each non-empty row of A enters the front owning its LEFTMOST column. Iterating
    // columns ascending, the FIRST column containing row i is i's leftmost column.
    crd::containers::Array<crd::u32> leftcol(alloc);
    leftcol.resize(static_cast<crd::usize>(m));
    for (crd::u32 i = 0; i < m; ++i)
    {
        leftcol[i] = n; // sentinel: empty row
    }
    const crd::u32* ap = a.outer_ptr.data();
    const crd::u32* ai = a.inner_idx.data();
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 p = ap[k]; p < ap[k + 1]; ++p)
        {
            const crd::u32 i = ai[p];
            if (leftcol[i] == n)
            {
                leftcol[i] = k;
            }
        }
    }
    crd::u32 nempty = 0;
    for (crd::u32 i = 0; i < m; ++i)
    {
        if (leftcol[i] == n)
        {
            ++nempty;
        }
        else
        {
            ++out.sleft[leftcol[i] + 1];
        }
    }
    out.n_empty_rows = nempty;
    for (crd::u32 j = 0; j < n; ++j)
    {
        out.sleft[j + 1] += out.sleft[j];
    }
    out.row_by_leftcol.resize(static_cast<crd::usize>(m) - nempty);
    crd::containers::Array<crd::u32> wcur(alloc);
    wcur.resize(static_cast<crd::usize>(n));
    for (crd::u32 j = 0; j < n; ++j)
    {
        wcur[j] = out.sleft[j];
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        if (leftcol[i] != n)
        {
            const crd::u32 j = leftcol[i];
            out.row_by_leftcol[wcur[j]++] = i;
        }
    }

    // 4. Dense R-block area (the numeric panel reserve): each front stores an nc × |fn| upper block.
    crd::u64 store = 0;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u64 nc = out.fronts.scol[f + 1] - out.fronts.scol[f];
        const crd::u64 fnsz = out.fronts.srowp[f + 1] - out.fronts.srowp[f];
        store += nc * fnsz;
    }
    out.rblock_storage = store;
    return out;
}

// =======================================================================
// MultifrontalQR<T> — numeric factor (v5c-1b).
// =======================================================================
template <typename T>
MultifrontalQR<T>::MultifrontalQR(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_sym(alloc), m_fb(alloc), m_fboff(alloc), m_fm(alloc), m_npiv(alloc), m_tau(alloc),
      m_tauoff(alloc), m_childp(alloc), m_child(alloc), m_rp(alloc), m_rj(alloc), m_rx(alloc), m_dead(alloc)
{
}

template <typename T>
void MultifrontalQR<T>::factorize(const sparse::SparsePattern& pat, crd::containers::ConstSpan<T> values,
                                  crd::u32 nrelax, crd::u32 num_workers)
{
    CRD_ASSERT_MSG(pat.is_compressed(), "MultifrontalQR requires a compressed CSC pattern");
    CRD_ASSERT_MSG(pat.format == sparse::SparseFormat::Csc, "MultifrontalQR requires CSC");
    CRD_ASSERT_MSG(values.size() == pat.inner_idx.size(), "MultifrontalQR: values/pattern size mismatch");

    m_m = pat.rows;
    m_n = pat.cols;
    m_info = 0;
    m_rnnz = 0;
    m_hnnz = 0;
    m_sym = multifrontal_qr_symbolic(pat, m_alloc, nrelax);
    const crd::u32 nf = m_sym.nf();
    const crd::u32 m = m_m;
    const crd::u32 n = m_n;

    m_dead.resize(static_cast<crd::usize>(n)); // value-init 0 (1 = rank-deficient pivot); rank set at R-build
    for (crd::u32 r = 0; r < n; ++r)
    {
        m_dead[r] = 0;
    }
    m_rank = n;

    m_rp.resize(static_cast<crd::usize>(n) + 1); // value-init 0
    if (nf == 0 || n == 0)
    {
        return;
    }

    // --- CSR of A WITH VALUES (row access for the front scatter) ---
    const crd::u32* ap = pat.outer_ptr.data();
    const crd::u32* ai = pat.inner_idx.data();
    const crd::usize nnz = pat.inner_idx.size();
    crd::containers::Array<crd::u32> arp(m_alloc);
    arp.resize(static_cast<crd::usize>(m) + 1);
    for (crd::usize p = 0; p < nnz; ++p)
    {
        ++arp[ai[p] + 1];
    }
    for (crd::u32 i = 0; i < m; ++i)
    {
        arp[i + 1] += arp[i];
    }
    crd::containers::Array<crd::u32> aci(m_alloc);
    aci.resize(nnz);
    crd::containers::Array<T> ax(m_alloc);
    ax.resize(nnz);
    crd::containers::Array<crd::u32> rcur(m_alloc);
    rcur.resize(static_cast<crd::usize>(m));
    for (crd::u32 i = 0; i < m; ++i)
    {
        rcur[i] = arp[i];
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        for (crd::u32 p = ap[k]; p < ap[k + 1]; ++p)
        {
            const crd::u32 i = ai[p];
            aci[rcur[i]] = k;
            ax[rcur[i]] = values[p];
            ++rcur[i];
        }
    }

    // --- front children (CSR over the front tree); children appended in ascending front index ---
    m_childp.resize(static_cast<crd::usize>(nf) + 1); // value-init 0
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 par = m_sym.front_parent[f];
        if (par != ordering::kNoParent)
        {
            ++m_childp[par + 1];
        }
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        m_childp[f + 1] += m_childp[f];
    }
    m_child.resize(m_childp[nf]);
    crd::containers::Array<crd::u32> ccur(m_alloc);
    ccur.resize(static_cast<crd::usize>(nf));
    for (crd::u32 f = 0; f < nf; ++f)
    {
        ccur[f] = m_childp[f];
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 par = m_sym.front_parent[f];
        if (par != ordering::kNoParent)
        {
            m_child[ccur[par]++] = f;
        }
    }

    const auto fnsz = [&](crd::u32 f)
    {
        return m_sym.fronts.srowp[f + 1] - m_sym.fronts.srowp[f];
    };

    // --- pass 1: fm + npiv per front (postorder; children resolved before parents) ---
    m_fm.resize(static_cast<crd::usize>(nf));
    m_npiv.resize(static_cast<crd::usize>(nf));
    for (crd::u32 idx = 0; idx < nf; ++idx)
    {
        const crd::u32 f = m_sym.front_post[idx];
        const crd::u32 nc = m_sym.fronts.scol[f + 1] - m_sym.fronts.scol[f];
        crd::u32 fmv = m_sym.sleft[m_sym.fronts.scol[f + 1]] - m_sym.sleft[m_sym.fronts.scol[f]];
        for (crd::u32 cc = m_childp[f]; cc < m_childp[f + 1]; ++cc)
        {
            const crd::u32 c = m_child[cc];
            fmv += m_fm[c] - m_npiv[c]; // child contribution-block rows
        }
        m_fm[f] = fmv;
        m_npiv[f] = (nc < fmv) ? nc : fmv; // npiv = min(nc, fm)
    }

    // --- offsets + storage ---
    m_fboff.resize(static_cast<crd::usize>(nf) + 1);
    m_tauoff.resize(static_cast<crd::usize>(nf) + 1);
    m_fboff[0] = 0;
    m_tauoff[0] = 0;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        m_fboff[f + 1] = m_fboff[f] + m_fm[f] * fnsz(f);
        m_tauoff[f + 1] = m_tauoff[f] + m_npiv[f];
    }
    m_fb.resize(m_fboff[nf]); // value-init 0 (scatter-PLACE into zeros)
    m_tau.resize(m_tauoff[nf]);

    // Per-front scratch sizing.
    crd::u32 maxfm = 1;
    crd::u32 maxfsz = 1;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        if (m_fm[f] > maxfm)
        {
            maxfm = m_fm[f];
        }
        const crd::u32 fs = m_sym.fronts.srowp[f + 1] - m_sym.fronts.srowp[f];
        if (fs > maxfsz)
        {
            maxfsz = fs;
        }
    }

    // Tree-parallel (v5c): level-schedule the front tree and factor independent fronts in parallel.
    // DETERMINISM MOAT — each front's QR is a pure local function of the symbolic + its children's
    // contribution blocks (read-only; finished in a strictly-lower level, published by the per-level
    // jobs::wait barrier), and the assembly order (own rows by leftmost column, then children in
    // ascending front-tree order) is FIXED ⇒ R and the Householder vectors are bit-identical across
    // worker counts. A front is read ONLY by its direct parent (contribution columns ⊆ parent fn), so a
    // level barrier dominates all readers. Scratch is per-worker (disjoint slices, worker_index()-keyed
    // per feedback_jobs_worker_index_aliasing); the larfb gemms pass scratch=nullptr → the per-thread
    // pooled GrowableTlsf (thread-safe + result-identical). sw=1 ⇒ serial (no jobs dispatch).
    const crd::u32 sw = (num_workers <= 1) ? 1U : crd::jobs::num_workers();
    const crd::usize vbuf_stride = static_cast<crd::usize>(maxfm) * kQrPanelW;  // V panel
    const crd::usize nbnb = static_cast<crd::usize>(kQrPanelW) * kQrPanelW;     // VᵀV / compact-WY T
    const crd::usize wbuf_stride = static_cast<crd::usize>(kQrPanelW) * maxfsz; // W = VᵀC
    crd::containers::Array<T> vbuf_pool(m_alloc);
    crd::containers::Array<T> vtv_pool(m_alloc);
    crd::containers::Array<T> tblk_pool(m_alloc);
    crd::containers::Array<T> wbuf_pool(m_alloc);
    crd::containers::Array<crd::u32> colpos_pool(m_alloc); // scatter map (v5c-1f), per worker
    vbuf_pool.resize(vbuf_stride * sw);
    vtv_pool.resize(nbnb * sw);
    tblk_pool.resize(nbnb * sw);
    wbuf_pool.resize(wbuf_stride * sw);
    colpos_pool.resize(static_cast<crd::usize>(n) * sw);

    // Factor one front on worker wk. Pure function of the symbolic + already-factored children.
    auto factor_front = [&](crd::u32 f, crd::u32 wk)
    {
        const crd::u32 fsz = fnsz(f);
        const crd::u32 fmv = m_fm[f];
        const crd::u32* fn = (m_sym.fronts.srow.data() + m_sym.fronts.srowp[f]);
        T* fb = (m_fb.data() + m_fboff[f]); // column-major fmv × fsz, zeroed
        T* const vb = vbuf_pool.data() + static_cast<crd::usize>(wk) * vbuf_stride;
        T* const vt = vtv_pool.data() + static_cast<crd::usize>(wk) * nbnb;
        T* const tb = tblk_pool.data() + static_cast<crd::usize>(wk) * nbnb;
        T* const wb = wbuf_pool.data() + static_cast<crd::usize>(wk) * wbuf_stride;
        crd::u32* const cp = colpos_pool.data() + static_cast<crd::usize>(wk) * n;

        // Scatter map for this front: global column id → local index in `fn`. Only columns ∈ fn are
        // read below (own-row + child-contribution columns are ⊆ fn by the symbolic), so stale entries
        // from earlier fronts on this worker are never read ⇒ no reset needed.
        for (crd::u32 t = 0; t < fsz; ++t)
        {
            cp[fn[t]] = t;
        }

        // own A-rows (canonical order: row_by_leftcol over the front's pivot columns)
        crd::u32 prow = 0;
        const crd::u32 own_lo = m_sym.sleft[m_sym.fronts.scol[f]];
        const crd::u32 own_hi = m_sym.sleft[m_sym.fronts.scol[f + 1]];
        for (crd::u32 rr = own_lo; rr < own_hi; ++rr)
        {
            const crd::u32 gr = m_sym.row_by_leftcol[rr];
            for (crd::u32 q = arp[gr]; q < arp[gr + 1]; ++q)
            {
                const crd::u32 t = cp[aci[q]];
                CRD_ASSERT_MSG(t < fsz && fn[t] == aci[q],
                               "QR assembly: own A-row column absent from front (symbolic/assembly mismatch)");
                fb[static_cast<crd::usize>(t) * fmv + prow] = ax[q];
            }
            ++prow;
        }
        // children contribution blocks appended as new rows (scatter columns ⊆ parent fn)
        for (crd::u32 cc = m_childp[f]; cc < m_childp[f + 1]; ++cc)
        {
            const crd::u32 c = m_child[cc];
            const crd::u32 c_nc = m_sym.fronts.scol[c + 1] - m_sym.fronts.scol[c];
            const crd::u32 c_npiv = m_npiv[c];
            const crd::u32 c_fm = m_fm[c];
            const crd::u32 c_fsz = fnsz(c);
            const crd::u32* c_fn = m_sym.fronts.srow.data() + m_sym.fronts.srowp[c];
            const T* cb = m_fb.data() + m_fboff[c];
            for (crd::u32 crrow = c_npiv; crrow < c_fm; ++crrow) // contribution rows [npiv, fm)
            {
                for (crd::u32 j = c_nc; j < c_fsz; ++j) // contribution columns [nc, fsz)
                {
                    const crd::u32 gc = c_fn[j];
                    const crd::u32 t = cp[gc];
                    CRD_ASSERT_MSG(t < fsz && fn[t] == gc,
                                   "QR assembly: child contribution column absent from parent front");
                    fb[static_cast<crd::usize>(t) * fmv + prow] = cb[static_cast<crd::usize>(j) * c_fm + crrow];
                }
                ++prow;
            }
        }
        CRD_ASSERT_MSG(prow == fmv, "QR assembly: front row count mismatch");

        // Partial Householder QR over the first npiv pivot columns. SMALL fronts use the proven
        // unblocked path (apply each reflector to ALL trailing columns); LARGE fronts use the
        // compact-WY BLOCKED path (panel-factor nb columns, then ONE BLAS-3 larfb to the trailing
        // block) — the lever that makes the big dense near-root fronts tractable + fast.
        const crd::u32 npiv = m_npiv[f];
        T* tau = m_tau.data() + m_tauoff[f];
        // Real fronts ≥ kQrBlockMin take the compact-WY BLOCKED path; COMPLEX stays UNBLOCKED (v5c-2a):
        // blocked-WY-for-complex (VᴴV + ConjTranspose larfb + a conj-aware compact-WY T) is a PERF
        // follow-on, not a deferred feature — complex QR is correct at all sizes via the unblocked path.
        bool blocked = false;
        if constexpr (!dense::is_complex_v<T>)
        {
            blocked = (fmv >= kQrBlockMin);
        }
        for (crd::u32 ps = 0; ps < npiv; ps += kQrPanelW)
        {
            const crd::u32 pw = (npiv - ps < kQrPanelW) ? (npiv - ps) : kQrPanelW;
            const crd::u32 apply_end = blocked ? (ps + pw) : fsz; // within-panel only when blocked
            for (crd::u32 k = ps; k < ps + pw; ++k)
            {
                const crd::u32 len = fmv - k;
                T* colk = fb + static_cast<crd::usize>(k) * fmv + k; // contiguous rows k..fmv-1 (col-major)
                if (len <= 1)
                {
                    // No rows below the diagonal ⇒ a trivial reflector (H = I): R[k][k] = colk[0] as-is,
                    // nothing to apply. CRITICAL for complex — make_householder_complex's n≤1 branch returns
                    // only Re(α) (β is real-typed), which would drop the imaginary part of the last diagonal
                    // on an exactly-determined front (fm == npiv). Real is unaffected (β == colk[0] there).
                    tau[k] = T{};
                    continue;
                }
                T rtau;
                dense::RealType<T> rbeta;
                if constexpr (dense::is_complex_v<T>)
                {
                    const auto hh = dense::detail::make_householder_complex<dense::RealType<T>>(colk, len);
                    rtau = hh.tau;
                    rbeta = hh.beta;
                }
                else
                {
                    const auto hh = dense::detail::make_householder<T>(colk, len);
                    rtau = hh.tau;
                    rbeta = hh.beta;
                }
                tau[k] = rtau;
                // Apply the reflector to the trailing columns [k+1, apply_end) as an inlined fused dlarf.
                // REAL → Hₖ (Qᵀ); COMPLEX → Hₖᴴ (Qᴴ): qr_conj makes the dot vᴴc and the scalar conj(τ),
                // the v-tail update stays un-conjugated. For real every qr_conj is a no-op ⇒ bit-identical
                // to the proven real path (the determinism moat is unchanged). v5c-1f measured dense::gemv+
                // ger 2–4× SLOWER on these small panel blocks, so the inlined hand-rolled pass stays.
                for (crd::u32 j = k + 1; j < apply_end; ++j)
                {
                    T* colj = fb + static_cast<crd::usize>(j) * fmv + k;
                    T s = colj[0]; // v[0] == 1
                    for (crd::u32 i = 1; i < len; ++i)
                    {
                        s += qr_conj(colk[i]) * colj[i];
                    }
                    s *= qr_conj(rtau);
                    colj[0] -= s;
                    for (crd::u32 i = 1; i < len; ++i)
                    {
                        colj[i] -= s * colk[i];
                    }
                }
                colk[0] = qr_from_real<T>(rbeta); // R diagonal (real β); colk[1..] keeps the Householder v-tail
            }
            // Compact-WY BLOCKED trailing update — REAL ONLY (complex is always unblocked above; the
            // if constexpr keeps the real-τ build_block_t / non-conjugate-Transpose gemms from ever
            // instantiating for complex T — blocked-WY-complex is the v5c-2a perf follow-on).
            if constexpr (!dense::is_complex_v<T>)
            {
                if (!blocked)
                {
                    continue; // unblocked path already updated all trailing columns
                }
                // Compact-WY trailing update: C := (I − V·Tᵀ·Vᵀ)·C for columns [ps+pw, fsz), rows [ps, fmv).
                const crd::u32 tc = fsz - (ps + pw);
                if (tc == 0)
                {
                    continue;
                }
                const crd::u32 fmp = fmv - ps;       // rows of V and C
                for (crd::u32 jj = 0; jj < pw; ++jj) // extract V (unit lower-trapezoidal), col-major ld = fmp
                {
                    const T* src = fb + static_cast<crd::usize>(ps + jj) * fmv + ps;
                    T* dst = vb + static_cast<crd::usize>(jj) * fmp;
                    for (crd::u32 i = 0; i < fmp; ++i)
                    {
                        if (i < jj)
                        {
                            dst[i] = T{0}; // above the unit diagonal
                        }
                        else if (i == jj)
                        {
                            dst[i] = T{1}; // unit diagonal of the trapezoidal V
                        }
                        else
                        {
                            dst[i] = src[i]; // the stored Householder v-tail
                        }
                    }
                }
                using CV = dense::MatrixView<const T, dense::Layout::ColMajor>;
                using MV = dense::MatrixView<T, dense::Layout::ColMajor>;
                const CV vview(vb, fmp, pw, fmp);
                MV vtvview(vt, pw, pw, pw);
                dense::gemm<T, dense::Layout::ColMajor>(T{1}, vview, vview, T{0}, vtvview, dense::Trans::Transpose,
                                                        dense::Trans::None, nullptr); // VᵀV (symmetric)
                dense::detail::build_block_t_from_vtv<T>(vt, pw, tau, ps, pw, tb, pw);
                T* cptr = fb + static_cast<crd::usize>(ps + pw) * fmv + ps;
                const CV cview(cptr, fmp, tc, fmv);
                MV wview(wb, pw, tc, pw);
                dense::gemm<T, dense::Layout::ColMajor>(T{1}, vview, cview, T{0}, wview, dense::Trans::Transpose,
                                                        dense::Trans::None, nullptr); // W = VᵀC
                for (crd::u32 c = 0; c < tc; ++c) // W := Tᵀ·W (T upper-tri row-major; descending row = in place)
                {
                    T* wc = wb + static_cast<crd::usize>(c) * pw;
                    for (crd::u32 ii = pw; ii-- > 0;)
                    {
                        T acc = T{0};
                        for (crd::u32 kk = 0; kk <= ii; ++kk)
                        {
                            acc += tb[static_cast<crd::usize>(kk) * pw + ii] * wc[kk];
                        }
                        wc[ii] = acc;
                    }
                }
                MV cwrite(cptr, fmp, tc, fmv);
                const CV wconst(wb, pw, tc, pw);
                dense::gemm<T, dense::Layout::ColMajor>(T{-1}, vview, wconst, T{1}, cwrite, dense::Trans::None,
                                                        dense::Trans::None, nullptr); // C := C − V·W
            } // if constexpr (!is_complex) — the real-only blocked path
        }
    };

    // Assembly-tree levels: level[f] = 1 + max(level[child]) (0 for a leaf). front_post is a postorder
    // ⇒ children precede f, so a single front_post pass computes levels. Same-level fronts are mutually
    // independent; ascending-f order within a level ⇒ the schedule is worker-order-independent (the moat).
    crd::containers::Array<crd::u32> level(m_alloc);
    level.resize(nf);
    crd::u32 nlevels = 0;
    for (crd::u32 idx = 0; idx < nf; ++idx)
    {
        const crd::u32 f = m_sym.front_post[idx];
        crd::u32 lev = 0;
        for (crd::u32 cc = m_childp[f]; cc < m_childp[f + 1]; ++cc)
        {
            const crd::u32 cl = level[m_child[cc]] + 1;
            if (cl > lev)
            {
                lev = cl;
            }
        }
        level[f] = lev;
        if (lev + 1 > nlevels)
        {
            nlevels = lev + 1;
        }
    }
    crd::containers::Array<crd::u32> lvl_ptr(m_alloc);
    lvl_ptr.resize(static_cast<crd::usize>(nlevels) + 1); // value-init 0
    for (crd::u32 f = 0; f < nf; ++f)
    {
        ++lvl_ptr[level[f] + 1];
    }
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        lvl_ptr[l + 1] += lvl_ptr[l];
    }
    crd::containers::Array<crd::u32> lvl_list(m_alloc);
    lvl_list.resize(nf);
    {
        crd::containers::Array<crd::u32> wp(m_alloc);
        wp.resize(nlevels);
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            wp[l] = lvl_ptr[l];
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            lvl_list[wp[level[f]]++] = f; // ascending f within a level (the moat)
        }
    }

    // --- pass 2: numeric, level-by-level (within a level, fronts are independent ⇒ front-parallel) ---
    for (crd::u32 l = 0; l < nlevels; ++l)
    {
        const crd::u32 lo = lvl_ptr[l];
        const crd::u32 cnt = lvl_ptr[l + 1] - lo;
        if (sw <= 1)
        {
            for (crd::u32 t = 0; t < cnt; ++t)
            {
                factor_front(lvl_list[lo + t], 0);
            }
        }
        else
        {
            auto* counter = crd::jobs::parallel_for(cnt, num_workers,
                                                    [&](crd::u32 b, crd::u32 e)
                                                    {
                                                        const crd::u32 wk = crd::jobs::worker_index();
                                                        for (crd::u32 t = b; t < e; ++t)
                                                        {
                                                            factor_front(lvl_list[lo + t], wk);
                                                        }
                                                    });
            crd::jobs::wait(counter);
            crd::jobs::frame_reset(); // reclaim this level's JobDecls (deep trees issue many levels)
        }
    }

    // --- build the global R, CSR by ascending row (row r = pivot column r) ---
    for (crd::u32 r = 0; r < n; ++r)
    {
        const crd::u32 f = m_sym.fronts.col_super[r];
        const crd::u32 i = r - m_sym.fronts.scol[f];
        if (i < m_npiv[f])
        {
            m_rp[r + 1] = fnsz(f) - i; // upper part: columns fn[i..fsz)
        }
        else
        {
            m_rp[r + 1] = 0;        // structural rank deficiency: the front could not pivot this column
            m_dead[r] = crd::u8{1}; // → DEAD (v5c-2b folds structural + numerical into one dead set)
        }
    }
    for (crd::u32 r = 0; r < n; ++r)
    {
        m_rp[r + 1] += m_rp[r];
    }
    m_rnnz = m_rp[n];
    m_rj.resize(m_rnnz);
    m_rx.resize(m_rnnz);
    for (crd::u32 r = 0; r < n; ++r)
    {
        const crd::u32 f = m_sym.fronts.col_super[r];
        const crd::u32 i = r - m_sym.fronts.scol[f];
        if (i >= m_npiv[f])
        {
            continue;
        }
        const crd::u32 fsz = fnsz(f);
        const crd::u32 fmv = m_fm[f];
        const crd::u32* fn = (m_sym.fronts.srow.data() + m_sym.fronts.srowp[f]);
        const T* fb = (m_fb.data() + m_fboff[f]);
        crd::u32 w = m_rp[r];
        for (crd::u32 t = i; t < fsz; ++t)
        {
            m_rj[w] = fn[t];
            m_rx[w] = fb[static_cast<crd::usize>(t) * fmv + i];
            ++w;
        }
    }

    // --- v5c-2b rank detection (Heath — NO column pivoting ⇒ the fill order + the determinism moat
    // hold): a pivot column is DEAD when |R[r][r]| ≤ rcond·max|R diagonal| (rcond = max(m,n)·eps), or
    // structurally (no R row, flagged above). The scan is SERIAL over r=0..n-1 (fixed order) ⇒ the dead
    // set is a pure function of R, which is bit-identical across worker counts ⇒ rank() is worker-invariant.
    using RT = dense::RealType<T>;
    RT maxdiag = RT{0};
    for (crd::u32 r = 0; r < n; ++r)
    {
        if (m_rp[r] < m_rp[r + 1]) // has an R row ⇒ its first entry is the diagonal R[r][r]
        {
            const RT d = qr_abs<T>(m_rx[m_rp[r]]);
            if (d > maxdiag)
            {
                maxdiag = d;
            }
        }
    }
    const RT rcond = static_cast<RT>(m > n ? m : n) * std::numeric_limits<RT>::epsilon();
    const RT tol = rcond * maxdiag;
    crd::usize ndead = 0;
    for (crd::u32 r = 0; r < n; ++r)
    {
        bool is_dead = (m_dead[r] != 0); // structural
        if (!is_dead && (m_rp[r] == m_rp[r + 1] || qr_abs<T>(m_rx[m_rp[r]]) <= tol))
        {
            is_dead = true; // numerical (tiny diagonal) — or an empty row that slipped through
        }
        m_dead[r] = is_dead ? crd::u8{1} : crd::u8{0};
        if (is_dead)
        {
            ++ndead;
        }
    }
    m_rank = static_cast<crd::usize>(n) - ndead;

    crd::u64 hcount = 0;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 k = 0; k < m_npiv[f]; ++k)
        {
            hcount += (m_fm[f] - 1 - k); // stored v-tail length of reflector k
        }
    }
    m_hnnz = hcount;
}

template <typename T>
bool MultifrontalQR<T>::least_squares(crd::containers::ConstSpan<T> b, crd::containers::Span<T> x,
                                      crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return false; // reserved for a genuine factor failure (rank deficiency is normal — see below)
    }
    // v5c-2b: rank-deficient solve returns the BASIC solution (dead variables = 0). No early-out on
    // rank deficiency — that is reported via rank()/dead(); the back-substitution below skips dead rows.
    const crd::u32 m = m_m;
    const crd::u32 n = m_n;
    const crd::u32 nf = m_sym.nf();
    CRD_ASSERT_MSG(b.size() == static_cast<crd::usize>(m) * nrhs, "QR least_squares: b size mismatch (expect m·nrhs)");
    CRD_ASSERT_MSG(x.size() == static_cast<crd::usize>(n) * nrhs, "QR least_squares: x size mismatch (expect n·nrhs)");
    if (n == 0 || nrhs == 0)
    {
        return true;
    }

    // c[col, s] — the transformed RHS Qᵀ·b indexed by global pivot column. Each column is a
    // pivot of exactly one front ⇒ written exactly once across the walk (full rank).
    crd::containers::Array<T> c(m_alloc);
    c.resize(static_cast<crd::usize>(n) * nrhs);

    // Per-front leftover rows (the contribution-block workspace passed up to the parent), cm_f × nrhs.
    crd::containers::Array<crd::u32> looff(m_alloc);
    looff.resize(static_cast<crd::usize>(nf) + 1);
    looff[0] = 0;
    crd::u32 maxfm = 1;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 cm = m_fm[f] - m_npiv[f];
        looff[f + 1] = looff[f] + cm * static_cast<crd::u32>(nrhs);
        if (m_fm[f] > maxfm)
        {
            maxfm = m_fm[f];
        }
    }
    crd::containers::Array<T> lo(m_alloc);
    lo.resize(looff[nf]);
    crd::containers::Array<T> w(m_alloc); // reused front workspace, fm × nrhs (col-major within a front)
    w.resize(static_cast<crd::usize>(maxfm) * nrhs);

    // --- Qᵀ·b along the assembly tree (postorder), the canonical row order of the factor ---
    for (crd::u32 idx = 0; idx < nf; ++idx)
    {
        const crd::u32 f = m_sym.front_post[idx];
        const crd::u32 fmv = m_fm[f];
        const crd::u32 npiv = m_npiv[f];
        const T* fb = (m_fb.data() + m_fboff[f]);
        const T* tau = (m_tau.data() + m_tauoff[f]);

        // assemble the workspace in the SAME row order the factor used.
        crd::u32 prow = 0;
        const crd::u32 own_lo = m_sym.sleft[m_sym.fronts.scol[f]];
        const crd::u32 own_hi = m_sym.sleft[m_sym.fronts.scol[f + 1]];
        for (crd::u32 rr = own_lo; rr < own_hi; ++rr)
        {
            const crd::u32 gr = m_sym.row_by_leftcol[rr];
            for (crd::usize s = 0; s < nrhs; ++s)
            {
                w[s * fmv + prow] = b[s * m + gr];
            }
            ++prow;
        }
        for (crd::u32 cc = m_childp[f]; cc < m_childp[f + 1]; ++cc)
        {
            const crd::u32 cchild = m_child[cc];
            const crd::u32 cm = m_fm[cchild] - m_npiv[cchild];
            const T* clo = lo.data() + looff[cchild];
            for (crd::u32 cr = 0; cr < cm; ++cr)
            {
                for (crd::usize s = 0; s < nrhs; ++s)
                {
                    w[s * fmv + prow] = clo[s * cm + cr];
                }
                ++prow;
            }
        }

        // Apply the front's npiv reflectors to the workspace: w := Hₖᴴ·w (the SAME Qᴴ-apply as the factor,
        // re-walked in canonical order). v = [1, fb[k·fmv+k+1 ..]]. REAL: Hₖᴴ = Hₖ (qr_conj is a no-op).
        // COMPLEX: dot = vᴴw uses qr_conj(v), scalar uses qr_conj(τ); the v-tail update stays un-conjugated.
        for (crd::u32 k = 0; k < npiv; ++k)
        {
            const T* vk = &fb[static_cast<crd::usize>(k) * fmv]; // column k; vk[k+1..fmv) = v-tail
            const T tk = tau[k];
            for (crd::usize s = 0; s < nrhs; ++s)
            {
                T* wc = w.data() + s * fmv;
                T dot = wc[k]; // v[0] == 1
                for (crd::u32 i = k + 1; i < fmv; ++i)
                {
                    dot += qr_conj(vk[i]) * wc[i];
                }
                dot *= qr_conj(tk);
                wc[k] -= dot;
                for (crd::u32 i = k + 1; i < fmv; ++i)
                {
                    wc[i] -= dot * vk[i];
                }
            }
        }

        // top npiv rows → c for this front's pivot columns; trailing rows → leftover for the parent.
        const crd::u32 col0 = m_sym.fronts.scol[f];
        for (crd::usize s = 0; s < nrhs; ++s)
        {
            for (crd::u32 k = 0; k < npiv; ++k)
            {
                c[s * n + (col0 + k)] = w[s * fmv + k];
            }
        }
        const crd::u32 cm = fmv - npiv;
        T* flo = lo.data() + looff[f];
        for (crd::usize s = 0; s < nrhs; ++s)
        {
            for (crd::u32 cr = 0; cr < cm; ++cr)
            {
                flo[s * cm + cr] = w[s * fmv + (npiv + cr)];
            }
        }
    }

    // --- back-substitution R·x = c (R CSR, row r ascending columns, diagonal first). v5c-2b BASIC
    // solution: a DEAD column (rank-deficient pivot) is a free variable set to 0 — skip its row (no
    // divide by the ~0 diagonal). Live rows back-substitute; dead columns appearing in a live row's
    // off-diagonal contribute 0 (their x is 0). ---
    for (crd::u32 rr = n; rr-- > 0;)
    {
        if (m_dead[rr] != 0)
        {
            for (crd::usize s = 0; s < nrhs; ++s)
            {
                x[s * n + rr] = T{}; // basic solution: dead variable = 0
            }
            continue;
        }
        const crd::u32 diag_pos = m_rp[rr];
        const T diag = m_rx[diag_pos];
        for (crd::usize s = 0; s < nrhs; ++s)
        {
            T acc = c[s * n + rr];
            for (crd::u32 a = diag_pos + 1; a < m_rp[rr + 1]; ++a)
            {
                acc -= m_rx[a] * x[s * n + m_rj[a]];
            }
            x[s * n + rr] = acc / diag;
        }
    }
    return true;
}

template <typename T> bool MultifrontalQR<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    CRD_ASSERT_MSG(m_m == m_n, "MultifrontalQR::solve is the square A·X=B path; use least_squares for m>n");
    // b and x alias `rhs`: the Qᵀ walk reads all of b into workspaces before back-substitution writes x.
    return least_squares(crd::containers::ConstSpan<T>{rhs.data(), rhs.size()}, rhs, nrhs);
}

template <typename T>
MultifrontalQR<T> factor_multifrontal_qr(const sparse::SparsePattern& pattern, crd::containers::ConstSpan<T> values,
                                         crd::memory::IAllocator* alloc, crd::u32 nrelax, crd::u32 num_workers)
{
    MultifrontalQR<T> qr(alloc);
    qr.factorize(pattern, values, nrelax, num_workers);
    return qr;
}

template class MultifrontalQR<crd::f32>;
template class MultifrontalQR<crd::f64>;
template class MultifrontalQR<Complex32>; // v5c-2a: complex QR (A·P = Q·R, Qᴴ-apply), unblocked path
template class MultifrontalQR<Complex64>;
template MultifrontalQR<crd::f32> factor_multifrontal_qr<crd::f32>(const sparse::SparsePattern&,
                                                                   crd::containers::ConstSpan<crd::f32>,
                                                                   crd::memory::IAllocator*, crd::u32, crd::u32);
template MultifrontalQR<crd::f64> factor_multifrontal_qr<crd::f64>(const sparse::SparsePattern&,
                                                                   crd::containers::ConstSpan<crd::f64>,
                                                                   crd::memory::IAllocator*, crd::u32, crd::u32);
template MultifrontalQR<Complex32> factor_multifrontal_qr<Complex32>(const sparse::SparsePattern&,
                                                                     crd::containers::ConstSpan<Complex32>,
                                                                     crd::memory::IAllocator*, crd::u32, crd::u32);
template MultifrontalQR<Complex64> factor_multifrontal_qr<Complex64>(const sparse::SparsePattern&,
                                                                     crd::containers::ConstSpan<Complex64>,
                                                                     crd::memory::IAllocator*, crd::u32, crd::u32);

} // namespace crd::hesap::direct
