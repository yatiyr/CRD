#include <crd/core/assert.hpp>
#include <crd/hesap/direct/lu_solve.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/ordering/symbolic.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>
#include <crd/memory/allocators/thread_safe_allocator.hpp>

#include <cmath>
#include <cstdio> // CRD_MF_PROFILE diagnostic (front-size + flop distribution)
#include <cstdlib>
#include <limits>
#include <utility>

namespace crd::hesap::direct
{
namespace
{
// Element-growth ratio max|stored L,U| / ‖B‖ above which the natural-diagonal static LU is deemed unstable ⇒
// fall back to MC64. The separation is astronomical (stable CFD/saddle-point stay O(1)–O(1e2); genuinely-
// unsymmetric/circuit blow up to ~1e160), so the value is not sensitive: 1e12 sits comfortably above any
// legitimate growth (≫ the 1/√ε ≈ 6.7e7 half-precision-loss line) and far below the blow-ups.
constexpr double kMfGrowthThreshold = 1e12;

// Is every element of the ascending `sub` array present in the ascending `sup` array?
// O(sub + sup) two-pointer (the extend_add subset contract — child ids subset parent ids).
[[nodiscard]] bool ascending_subset(const crd::u32* sub, crd::u32 sn, const crd::u32* sup, crd::u32 supn) noexcept
{
    crd::u32 j = 0;
    for (crd::u32 i = 0; i < sn; ++i)
    {
        while (j < supn && sup[j] < sub[i])
        {
            ++j;
        }
        if (j >= supn || sup[j] != sub[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

MultifrontalSymbolic build_multifrontal_symbolic(const LuSymbolic& sym, crd::memory::IAllocator* alloc)
{
    MultifrontalSymbolic mf(alloc);
    const crd::u32 n = sym.n;
    const crd::u32 nf = sym.nsuper;
    mf.n = n;
    mf.nfront = nf;

    // pivot_first = the supernode boundaries (front f pivots = [super[f], super[f+1])).
    mf.pivot_first.resize(static_cast<crd::usize>(nf) + 1);
    for (crd::u32 f = 0; f <= nf; ++f)
    {
        mf.pivot_first[f] = sym.super[f];
    }
    if (nf == 0)
    {
        mf.row_ptr.push_back(0U);
        mf.col_ptr.push_back(0U);
        return mf;
    }

    // column -> front map.
    crd::containers::Array<crd::u32> col_front(alloc);
    col_front.resize(n);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 c = sym.super[f]; c < sym.super[f + 1]; ++c)
        {
            col_front[c] = f;
        }
    }

    // front_parent[f] = front of col_etree[last pivot col of f] (kNoParent at a root).
    mf.front_parent.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 last = sym.super[f + 1] - 1;
        const crd::u32 p = sym.col_etree[last];
        mf.front_parent[f] = (p == ordering::kNoParent) ? ordering::kNoParent : col_front[p];
    }

    // rows[f] = the L row pattern of the leading column (a dense-trapezoid superset of every
    // member column): pivots [c0,c1) first, then the L foot — already ascending.
    mf.row_ptr.resize(static_cast<crd::usize>(nf) + 1);
    mf.row_ptr[0] = 0U;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 c0 = sym.super[f];
        for (crd::u32 p = sym.lp[c0]; p < sym.lp[c0 + 1]; ++p)
        {
            mf.row_idx.push_back(sym.li[p]);
        }
        mf.row_ptr[f + 1] = static_cast<crd::u32>(mf.row_idx.size());
    }

    // cols[f] = pivots union {U columns the front's pivot rows touch}. U is CSC (up/ui): a pivot
    // row i touches column k iff i in U(:,k). Two monotonic passes over U build ascending, dedup'd
    // per-front column lists (k increases ⇒ ascending; last_col[f] dedups a column reached by
    // several pivot rows of the same front in one k). The pivots are included via U(k,k)'s diagonal.
    crd::containers::Array<crd::u32> col_count(alloc);
    col_count.resize(nf); // value-init 0
    crd::containers::Array<crd::u32> last_col(alloc);
    last_col.resize(nf);
    constexpr crd::u32 no_col = 0xFFFFFFFFU;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        last_col[f] = no_col;
    }
    for (crd::u32 k = 0; k < n; ++k) // pass A: count
    {
        for (crd::u32 p = sym.up[k]; p < sym.up[k + 1]; ++p)
        {
            const crd::u32 f = col_front[sym.ui[p]];
            if (last_col[f] != k)
            {
                ++col_count[f];
                last_col[f] = k;
            }
        }
    }
    mf.col_ptr.resize(static_cast<crd::usize>(nf) + 1);
    mf.col_ptr[0] = 0U;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        mf.col_ptr[f + 1] = mf.col_ptr[f] + col_count[f];
    }
    mf.col_idx.resize(mf.col_ptr[nf]);
    crd::containers::Array<crd::u32> cursor(alloc);
    cursor.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        cursor[f] = mf.col_ptr[f];
        last_col[f] = no_col;
    }
    for (crd::u32 k = 0; k < n; ++k) // pass B: fill
    {
        for (crd::u32 p = sym.up[k]; p < sym.up[k + 1]; ++p)
        {
            const crd::u32 f = col_front[sym.ui[p]];
            if (last_col[f] != k)
            {
                mf.col_idx[cursor[f]++] = k;
                last_col[f] = k;
            }
        }
    }

    return mf;
}

MultifrontalSymbolic build_symmetric_multifrontal_symbolic(const sparse::SparsePattern& b,
                                                           crd::memory::IAllocator* alloc)
{
    // chol(B+Bᵀ) symbolic: etree + fundamental supernodes + full L pattern (build_adjacency symmetrises B).
    const ordering::SymbolicFactor sf = ordering::symbolic_factorize(b, alloc);
    MultifrontalSymbolic mf(alloc);
    const crd::u32 n = sf.n;
    const crd::u32 nf = sf.nsuper;
    mf.n = n;
    mf.nfront = nf;

    mf.pivot_first.resize(static_cast<crd::usize>(nf) + 1);
    for (crd::u32 f = 0; f <= nf; ++f)
    {
        mf.pivot_first[f] = sf.super[f];
    }
    if (nf == 0)
    {
        mf.row_ptr.push_back(0U);
        mf.col_ptr.push_back(0U);
        return mf;
    }

    crd::containers::Array<crd::u32> col_front(alloc);
    col_front.resize(n);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 c = sf.super[f]; c < sf.super[f + 1]; ++c)
        {
            col_front[c] = f;
        }
    }

    // front_parent[f] = front of the symmetric etree parent of f's last pivot column (always > f).
    mf.front_parent.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 last = sf.super[f + 1] - 1;
        const crd::u32 p = sf.parent[last];
        mf.front_parent[f] = (p == ordering::kNoParent) ? ordering::kNoParent : col_front[p];
    }

    // rows == cols == the SYMMETRIC supernode pattern (the leading column's L row pattern: a dense
    // trapezoid superset of every member column; pivots [c0,c1) first, then the symmetric foot — ascending).
    mf.row_ptr.resize(static_cast<crd::usize>(nf) + 1);
    mf.col_ptr.resize(static_cast<crd::usize>(nf) + 1);
    mf.row_ptr[0] = 0U;
    mf.col_ptr[0] = 0U;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 c0 = sf.super[f];
        for (crd::u32 p = sf.lp[c0]; p < sf.lp[c0 + 1]; ++p)
        {
            const crd::u32 r = sf.li[p];
            mf.row_idx.push_back(r);
            mf.col_idx.push_back(r); // symmetric front: column extent == row extent
        }
        mf.row_ptr[f + 1] = static_cast<crd::u32>(mf.row_idx.size());
        mf.col_ptr[f + 1] = static_cast<crd::u32>(mf.col_idx.size());
    }
    return mf;
}

MfContainmentReport check_multifrontal_containment(const MultifrontalSymbolic& mf)
{
    MfContainmentReport rep;
    rep.nfront = mf.nfront;
    for (crd::u32 f = 0; f < mf.nfront; ++f)
    {
        const crd::u32 nr = mf.row_ptr[f + 1] - mf.row_ptr[f];
        const crd::u32 nc = mf.col_ptr[f + 1] - mf.col_ptr[f];
        rep.max_front_rows = (nr > rep.max_front_rows) ? nr : rep.max_front_rows;
        rep.max_front_cols = (nc > rep.max_front_cols) ? nc : rep.max_front_cols;

        const crd::u32 p = mf.front_parent[f];
        if (p == ordering::kNoParent)
        {
            continue;
        }
        ++rep.nchild;
        const crd::u32 npiv = mf.npiv(f);
        // Contribution block = the front extent past the pivots (rows/cols are pivots-first, ascending).
        const crd::u32* cb_rows = mf.row_idx.data() + mf.row_ptr[f] + npiv;
        const crd::u32 cb_nrows = nr - npiv;
        const crd::u32* cb_cols = mf.col_idx.data() + mf.col_ptr[f] + npiv;
        const crd::u32 cb_ncols = nc - npiv;
        const crd::u32* prow = mf.row_idx.data() + mf.row_ptr[p];
        const crd::u32 p_nrows = mf.row_ptr[p + 1] - mf.row_ptr[p];
        const crd::u32* pcol = mf.col_idx.data() + mf.col_ptr[p];
        const crd::u32 p_ncols = mf.col_ptr[p + 1] - mf.col_ptr[p];
        if (!ascending_subset(cb_rows, cb_nrows, prow, p_nrows))
        {
            ++rep.row_violations;
        }
        if (!ascending_subset(cb_cols, cb_ncols, pcol, p_ncols))
        {
            ++rep.col_violations;
        }
    }
    return rep;
}

// =======================================================================
// v5b-3b-3 — MultifrontalLU<T> driver (serial; tree-parallel in v5b-3c).
// =======================================================================

template <typename T>
MultifrontalLU<T>::MultifrontalLU(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_scale(alloc), m_b(alloc), m_lp(alloc), m_li(alloc), m_lx(alloc), m_up(alloc), m_ui(alloc),
      m_ux(alloc)
{
}

template <typename T>
double MultifrontalLU<T>::factor_attempt(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                         crd::u32 num_workers, bool use_mc64)
{
    m_n = a.pattern().rows;
    m_info = 0;
    const crd::u32 n = m_n; // factorize() guarantees n > 0 before calling

    m_lp.clear();
    m_li.clear();
    m_lx.clear();
    m_up.clear();
    m_ui.clear();
    m_ux.clear();

    // 1. Static-pivot transform → B (CSC, matched on diagonal) + D_r/D_c/perm. ADAPTIVE: MC64 when use_mc64,
    //    else the natural diagonal — both deterministic ⇒ the moat holds; the caller watches element growth.
    m_scale = static_lu_prepare<T>(a, m_b, m_alloc, use_mc64);

    // 2. Symmetric-pattern (chol(B+Bᵀ)) multifrontal symbolic — fronts with rows == cols, pivots-first.
    MultifrontalSymbolic mf = build_symmetric_multifrontal_symbolic(m_b.pattern(), m_alloc);
    m_nfront = mf.nfront;
    const crd::u32 nf = mf.nfront;

    // CSC (column scatter, L side) + CSR (row scatter, U side) views of B.
    const crd::u32* bp = m_b.pattern().outer_ptr.data();
    const crd::u32* bi = m_b.pattern().inner_idx.data();
    const T* bx = m_b.values().values.data();
    auto bcsr = sparse::from_csc<T>(m_b, m_alloc);
    const crd::u32* csr_ptr = bcsr.pattern().outer_ptr.data();
    const crd::u32* csr_idx = bcsr.pattern().inner_idx.data();
    const T* csr_val = bcsr.values().values.data();

    // GESP perturbation threshold √ε·‖B‖ (deterministic ⇒ moat-safe).
    dense::RealType<T> bnorm = dense::RealType<T>(0);
    const crd::u32 bnnz = static_cast<crd::u32>(m_b.pattern().inner_idx.size());
    for (crd::u32 p = 0; p < bnnz; ++p)
    {
        const dense::RealType<T> m = lu2_mag<T>(bx[p]);
        if (m > bnorm)
        {
            bnorm = m;
        }
    }
    const dense::RealType<T> eps = std::numeric_limits<dense::RealType<T>>::epsilon();
    const dense::RealType<T> tiny = std::sqrt(eps) * (bnorm > dense::RealType<T>(0) ? bnorm : dense::RealType<T>(1));

    // column → front map (pivots are contiguous per front; local pivot index = c - pivot_first[f]).
    crd::containers::Array<crd::u32> col_front(m_alloc);
    col_front.resize(n);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        for (crd::u32 c = mf.pivot_first[f]; c < mf.pivot_first[f + 1]; ++c)
        {
            col_front[c] = f;
        }
    }

    // children lists (CSR over fronts), appended in ASCENDING f order ⇒ fixed postorder extend-add = the moat.
    crd::containers::Array<crd::u32> chld_ptr(m_alloc);
    chld_ptr.resize(static_cast<crd::usize>(nf) + 1);
    crd::containers::Array<crd::u32> chld_cnt(m_alloc);
    chld_cnt.resize(nf); // value-init 0
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 par = mf.front_parent[f];
        if (par != ordering::kNoParent)
        {
            ++chld_cnt[par];
        }
    }
    chld_ptr[0] = 0;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        chld_ptr[f + 1] = chld_ptr[f] + chld_cnt[f];
    }
    crd::containers::Array<crd::u32> chld_idx(m_alloc);
    chld_idx.resize(chld_ptr[nf]);
    crd::containers::Array<crd::u32> chld_cur(m_alloc);
    chld_cur.resize(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        chld_cur[f] = chld_ptr[f];
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 par = mf.front_parent[f];
        if (par != ordering::kNoParent)
        {
            chld_idx[chld_cur[par]++] = f;
        }
    }

    // 3. L CSC structure (per column from its front: rows row_idx[jj..nr), diagonal == c first, ascending).
    m_lp.resize(static_cast<crd::usize>(n) + 1);
    m_lp[0] = 0;
    for (crd::u32 c = 0; c < n; ++c)
    {
        const crd::u32 f = col_front[c];
        const crd::u32 nr = mf.row_ptr[f + 1] - mf.row_ptr[f];
        const crd::u32 jj = c - mf.pivot_first[f];
        m_lp[c + 1] = m_lp[c] + (nr - jj);
    }
    m_lnz = m_lp[n];
    // Cheap pre-numeric bail: if the natural-diagonal factor is heading toward DENSE (a pathology like
    // gemat11, whose natural ordering explodes the chol(B+Bᵀ) fill), skip the expensive structure build +
    // numeric and let factorize() fall back to MC64 immediately. Fraction-of-dense is SCALE-SAFE — large sparse
    // 3D sim targets get sparser by this measure as n grows (fill ~n^4/3 ≪ n²/2), so they never false-trigger.
    // Only on the optimistic natural attempt; the MC64 fallback always runs.
    if (!use_mc64)
    {
        const double dense_l = 0.5 * static_cast<double>(n) * static_cast<double>(n);
        if (static_cast<double>(m_lnz) > 0.20 * dense_l)
        {
            return std::numeric_limits<double>::infinity(); // → factorize() re-runs with MC64
        }
    }
    m_li.resize_uninitialized(static_cast<crd::usize>(m_lnz)); // fully written by the per-column fill below
    for (crd::u32 c = 0; c < n; ++c)
    {
        const crd::u32 f = col_front[c];
        const crd::u32 nr = mf.row_ptr[f + 1] - mf.row_ptr[f];
        const crd::u32 jj = c - mf.pivot_first[f];
        const crd::u32* rows = mf.row_idx.data() + mf.row_ptr[f];
        crd::u32 w = m_lp[c];
        for (crd::u32 t = jj; t < nr; ++t)
        {
            m_li[w++] = rows[t];
        }
    }

    // 4. U CSC structure (column k accumulates pivot rows across fronts; ascending f ⇒ ascending rows,
    //    k's own front last ⇒ the diagonal U(k,k) lands last per column, as lu_lu_solve requires).
    m_up.resize(static_cast<crd::usize>(n) + 1);
    for (crd::u32 i = 0; i <= n; ++i)
    {
        m_up[i] = 0;
    }
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 npiv = mf.npiv(f);
        const crd::u32 nc = mf.col_ptr[f + 1] - mf.col_ptr[f];
        const crd::u32* cols = mf.col_idx.data() + mf.col_ptr[f];
        for (crd::u32 jc = 0; jc < nc; ++jc)
        {
            const crd::u32 cnt = (jc + 1 < npiv) ? (jc + 1) : npiv; // pivot rows ii in [0, min(jc+1, npiv))
            m_up[cols[jc] + 1] += cnt;
        }
    }
    for (crd::u32 k = 0; k < n; ++k)
    {
        m_up[k + 1] += m_up[k];
    }
    m_unz = m_up[n];
    m_ui.resize_uninitialized(static_cast<crd::usize>(m_unz)); // fully written by the per-(front,col) fill below
    crd::containers::Array<crd::u32> ucur(m_alloc);
    ucur.resize(n);
    for (crd::u32 k = 0; k < n; ++k)
    {
        ucur[k] = m_up[k];
    }
    // uoff[col_ptr[f]+jc] = the FIXED m_ux/m_ui offset where front f's column-jc U entries start. Recording it
    // in this serial ascending-front pass turns the value pass's racing `ucur[k]++` into disjoint per-front
    // writes ⇒ the tree-parallel front walk (v5b-3c) is race-free + worker-count-independent (the moat).
    crd::containers::Array<crd::u32> uoff(m_alloc);
    uoff.resize(static_cast<crd::usize>(mf.col_ptr[nf]));
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 npiv = mf.npiv(f);
        const crd::u32 nc = mf.col_ptr[f + 1] - mf.col_ptr[f];
        const crd::u32* cols = mf.col_idx.data() + mf.col_ptr[f];
        const crd::u32 c0 = mf.pivot_first[f];
        const crd::u32 ubase = mf.col_ptr[f];
        for (crd::u32 jc = 0; jc < nc; ++jc)
        {
            const crd::u32 k = cols[jc];
            const crd::u32 cnt = (jc + 1 < npiv) ? (jc + 1) : npiv;
            uoff[ubase + jc] = ucur[k]; // fixed start offset for this (front, column)
            for (crd::u32 ii = 0; ii < cnt; ++ii)
            {
                m_ui[ucur[k]++] = c0 + ii;
            }
        }
    }

    // INVARIANT (lu_lu_solve depends on it): U(k,k) is the LAST entry of column k. Holds because the
    // front owning k as a pivot is the highest-indexed front touching column k (any front emitting k as a
    // CB-column has all pivots < k ⇒ a lower index), and within that front the diagonal ii==jc is emitted
    // last. Assert it (debug-only) so any future structural change fails loudly instead of dividing wrong.
    for (crd::u32 k = 0; k < n; ++k)
    {
        CRD_ASSERT_MSG(m_up[k + 1] > m_up[k] && m_ui[m_up[k + 1] - 1] == k,
                       "MultifrontalLU: U(k,k) is not the last entry of column k (lu_lu_solve precondition)");
    }

    // Uninitialized: the numeric walk writes EVERY position of L (each pivot column's [m_lp[c],m_lp[c+1]))
    // and U (each (front,column)'s uoff slice) before any read ⇒ the zero-init of ~m_lnz+m_unz elements was
    // pure waste (~40 ms on ns3Da's 18M+18M fill). Bit-identical (every cell overwritten before use).
    m_lx.resize_uninitialized(static_cast<crd::usize>(m_lnz));
    m_ux.resize_uninitialized(static_cast<crd::usize>(m_unz));

    // 5. Numeric front walk (ascending f = postorder). Contribution blocks (Schur) kept per front; consumed
    //    by the parent (always a higher f). `loc` maps global id → front-local index (rows == cols symmetric).
    // cb[f] = front f's FULL factored buffer, kept alive until its parent consumes it; the contribution
    // block (Schur) is the trailing [cb_npiv[f]:, cb_npiv[f]:] block, read IN PLACE (no copy) by the parent.
    // Front buffers (cb[], working fronts, recycle pools) migrate across workers — a child factored by one
    // worker is consumed + recycled by its parent's worker ⇒ a ThreadSafeAllocator over m_alloc guards them.
    // DECLARED FIRST so it OUTLIVES every container of ts-allocated MfFronts (cb / cb_free): ~MfFront's element
    // Arrays call ts.deallocate, which must run BEFORE ~ts — else gcc traps "pure virtual method called" on the
    // destroyed allocator's vtable at scope exit (MSVC silently tolerated the dangling-vtable UB).
    crd::memory::ThreadSafeAllocator ts(m_alloc);
    crd::containers::Array<MfFront<T>> cb(m_alloc);   // cb[f] = front f's full factored buffer (Schur trailing)
    crd::containers::Array<crd::u32> cb_npiv(m_alloc); // cb_npiv[f] locates f's Schur block within cb[f]
    crd::containers::Array<crd::u32> loc(m_alloc);     // per-worker global id -> front-local index maps
    // Serial fallback for small problems: the fork-join + barrier overhead of the parallel walk exceeds the
    // numeric work below ~1M fill (e.g. circuit/oil matrices) ⇒ parallelism REGRESSES there (measured
    // sherman3 0.49x @ 8w). Bit-identical to the serial reference (the moat) since it IS the serial path.
    const bool small_problem = (m_lnz + m_unz) < (1ULL << 20);
    const crd::u32 sw = (num_workers <= 1 || small_problem) ? 1U : crd::jobs::num_workers();
    crd::u32 max_nr = 1;
    for (crd::u32 f = 0; f < nf; ++f)
    {
        const crd::u32 nrf = mf.row_ptr[f + 1] - mf.row_ptr[f];
        max_nr = nrf > max_nr ? nrf : max_nr;
    }
    constexpr crd::u32 no_loc = 0xFFFFFFFFU;

    // v5f-(a) within-front partial pivoting. do_pivot ⇒ each front records its getf2 swaps in a per-worker
    // ipiv buffer, applies them to the front's row_index, and writes the global row permutation P (m_rowperm);
    // a post-walk invperm pass remaps m_li from physical B-rows to elimination indices (the solve then applies
    // P). do_pivot == false ⇒ every line below is inert and the static path is byte-unchanged.
    const bool do_pivot = m_pivot_threshold > dense::RealType<T>(0);
    crd::containers::Array<crd::u32> ipiv_scr(m_alloc); // per-worker getf2 ipiv (length max_nr each)
    if (do_pivot)
    {
        m_rowperm.resize_uninitialized(n); // every column is a pivot in exactly one front ⇒ all n filled
        ipiv_scr.resize(static_cast<crd::usize>(sw) * max_nr);
    }

    // cb[f]/cb_npiv[f]: written by f's worker (own index), read by f's parent (higher level, after the
    // jobs::wait barrier). Pre-sized so the parallel walk never resizes the shared arrays.
    cb.reserve(nf);
    for (crd::u32 f = 0; f < nf; ++f)
    {
        cb.push_back(MfFront<T>(&ts));
    }
    cb_npiv.resize(nf);

    // Per-worker scratch, all pre-allocated so the parallel walk allocates ONLY front buffers (via ts):
    loc.resize(static_cast<crd::usize>(sw) * n); // global id → front-local index, per worker
    for (crd::usize i = 0; i < loc.size(); ++i)
    {
        loc[i] = no_loc;
    }
    crd::containers::Array<crd::containers::Array<crd::u32>> ea_rmap(m_alloc); // extend-add maps, per worker
    crd::containers::Array<crd::containers::Array<crd::u32>> ea_cmap(m_alloc);
    crd::containers::Array<crd::containers::Array<MfFront<T>>> cb_free(m_alloc); // recycle pools, per worker
    for (crd::u32 w = 0; w < sw; ++w)
    {
        ea_rmap.push_back(crd::containers::Array<crd::u32>(m_alloc));
        ea_cmap.push_back(crd::containers::Array<crd::u32>(m_alloc));
        ea_rmap[w].resize(max_nr); // capacity >= any child dim ⇒ resize-down during the walk never reallocs
        ea_cmap[w].resize(max_nr);
        cb_free.push_back(crd::containers::Array<MfFront<T>>(&ts));
    }
    // Per-worker GEMM pack-buffer arena (factor_front's dl::gemm; never the shared non-thread-safe TLSF). A
    // fresh LinearAllocator per front resets it; within a front it accumulates across panels (deallocate is a
    // no-op) ⇒ size for the per-front worst case 2·nr²·sizeof(T).
    crd::usize gemm_arena_bytes = 2 * static_cast<crd::usize>(max_nr) * static_cast<crd::usize>(max_nr) * sizeof(T) +
                                  static_cast<crd::usize>(max_nr) * 128 + (1U << 16);
    crd::containers::Array<crd::u8> gemm_scr(m_alloc);
    gemm_scr.resize(static_cast<crd::usize>(sw) * gemm_arena_bytes);

    // Per-worker element-growth accumulator: max |stored L,U entry| each worker sees. Reduced after the walk
    // into the growth ratio (vs ‖B‖) the adaptive MC64 fallback decides on. Per-worker ⇒ race-free + the
    // reduction is order-independent (max is associative+commutative) ⇒ the moat is unaffected.
    crd::containers::Array<dense::RealType<T>> wgrowth(m_alloc);
    wgrowth.resize(sw);
    for (crd::u32 w = 0; w < sw; ++w)
    {
        wgrowth[w] = dense::RealType<T>(0);
    }

    // Factor one front f on worker wk. Deterministic pure function of the symbolic + lower-level results ⇒
    // bit-identical across worker counts (the moat): per-worker scratch, fixed-postorder child assembly, and
    // fixed uoff/m_lp write positions ⇒ no data race + no order dependence.
    auto factor_one_front = [&](crd::u32 f, crd::u32 wk, bool gemm_par)
    {
        const crd::u32 nr = mf.row_ptr[f + 1] - mf.row_ptr[f]; // == nc (symmetric front)
        const crd::u32 npiv = mf.npiv(f);
        const crd::u32 c0 = mf.pivot_first[f];
        const crd::u32* rows = mf.row_idx.data() + mf.row_ptr[f];
        const crd::u32* cols = mf.col_idx.data() + mf.col_ptr[f];
        crd::u32* loc_w = loc.data() + static_cast<crd::usize>(wk) * n;
        crd::containers::Array<MfFront<T>>& pool = cb_free[wk];

        // Acquire a working buffer (reuse a recycled child's capacity; else fresh from ts).
        MfFront<T> front(&ts);
        if (pool.size() > 0)
        {
            front = std::move(pool[pool.size() - 1]);
            pool.pop_back();
        }
        front.resize(nr, nr);
        // Parallel zero-fill of the big near-root front buffer (gemm_par = the narrow main-thread path). The
        // serial memset of nr² (2.4M for nr=1561) was part of the singleton-front assembly cost; bit-identical
        // (zero is zero), bandwidth-bound ⇒ modest but free. Small fronts + the wide path keep the serial fill.
        {
            const crd::usize total = static_cast<crd::usize>(nr) * static_cast<crd::usize>(nr);
            // `jobs::num_workers()` evaluated LAST (short-circuit) so the serial path (gemm_par == false — e.g.
            // the one-shot CLI factor with no jobs::init) never touches the job system.
            if (gemm_par && total >= (crd::usize{1} << 18) && total <= 0xFFFFFFFFULL && crd::jobs::num_workers() > 1)
            {
                T* const d = front.data.data();
                crd::jobs::Counter* zc = crd::jobs::parallel_for(static_cast<crd::u32>(total),
                                                                 crd::jobs::num_workers(),
                                                                 [d](crd::u32 i0, crd::u32 i1) noexcept
                                                                 {
                                                                     for (crd::u32 i = i0; i < i1; ++i)
                                                                     {
                                                                         d[i] = T{0};
                                                                     }
                                                                 });
                crd::jobs::wait(zc);
            }
            else
            {
                front.zero_fill();
            }
        }
        for (crd::u32 i = 0; i < nr; ++i)
        {
            front.row_index[i] = rows[i];
            front.col_index[i] = cols[i];
            loc_w[rows[i]] = i;
        }

        // 5a. assemble original B entries (L pivot cols with row >= c; U pivot rows with col > prow).
        for (crd::u32 jj = 0; jj < npiv; ++jj)
        {
            const crd::u32 c = c0 + jj;
            for (crd::u32 p = bp[c]; p < bp[c + 1]; ++p)
            {
                const crd::u32 i = bi[p];
                if (i >= c)
                {
                    front.at(loc_w[i], jj) += bx[p];
                }
            }
        }
        for (crd::u32 ii = 0; ii < npiv; ++ii)
        {
            const crd::u32 prow = c0 + ii;
            for (crd::u32 p = csr_ptr[prow]; p < csr_ptr[prow + 1]; ++p)
            {
                const crd::u32 j = csr_idx[p];
                if (j > prow)
                {
                    front.at(ii, loc_w[j]) += csr_val[p];
                }
            }
        }

        // 5b. extend-add children's Schur blocks IN PLACE (no copy), fixed postorder = the moat, then recycle.
        for (crd::u32 cc = chld_ptr[f]; cc < chld_ptr[f + 1]; ++cc)
        {
            const crd::u32 g = chld_idx[cc];
            mf_extend_add_trailing<T>(front, cb[g], cb_npiv[g], ea_rmap[wk], ea_cmap[wk], gemm_par);
            pool.push_back(std::move(cb[g])); // recycle the child's buffer into this worker's pool
        }

        // 5c. factor (per-worker GEMM arena; fresh LinearAllocator resets the bump between fronts).
        crd::memory::LinearAllocator gemm_arena(gemm_scr.data() + static_cast<crd::usize>(wk) * gemm_arena_bytes,
                                                gemm_arena_bytes);
        crd::u32* const ipiv = do_pivot ? (ipiv_scr.data() + static_cast<crd::usize>(wk) * max_nr) : nullptr;
        factor_front<T>(front.data.data(), nr, nr, nr, npiv, tiny, &gemm_arena, gemm_par, m_pivot_threshold, ipiv,
                        &gemm_arena); // arena = the bump scratch ⇒ factor_front rewinds packs per serial schur
        if (do_pivot)
        {
            // Apply the getf2 swaps (in order) to the front's row_index so position t holds the PHYSICAL B-row
            // its factored data now belongs to; then record P[c0+jj] = the physical B-row pivoting column c0+jj.
            // Swaps are within [0, npiv) (restricted pivoting) ⇒ the contribution rows [npiv, nr) are untouched.
            for (crd::u32 k = 0; k < npiv; ++k)
            {
                if (ipiv[k] != k)
                {
                    const crd::u32 tmp = front.row_index[k];
                    front.row_index[k] = front.row_index[ipiv[k]];
                    front.row_index[ipiv[k]] = tmp;
                }
            }
            for (crd::u32 jj = 0; jj < npiv; ++jj)
            {
                m_rowperm[c0 + jj] = front.row_index[jj];
            }
        }

        // 5d. store L (own pivot columns ⇒ single-writer) + U (fixed uoff offsets ⇒ disjoint per front), while
        //     tracking the max |entry| for the adaptive MC64 growth check (near-free: piggybacks the stores).
        dense::RealType<T> fmax = wgrowth[wk];
        for (crd::u32 jj = 0; jj < npiv; ++jj)
        {
            const crd::u32 c = c0 + jj;
            crd::u32 w = m_lp[c];
            if (do_pivot)
            {
                m_li[w] = front.row_index[jj]; // PHYSICAL pivot row at the diagonal slot (post-walk invperm → c)
            }
            m_lx[w++] = lu2_from_real<T>(dense::RealType<T>(1)); // unit L diagonal
            for (crd::u32 t = jj + 1; t < nr; ++t)
            {
                if (do_pivot)
                {
                    m_li[w] = front.row_index[t]; // physical B-row at front position t (invperm → elim index)
                }
                const T lv = front.at(t, jj);
                m_lx[w++] = lv;
                const dense::RealType<T> am = lu2_mag<T>(lv);
                if (am > fmax)
                {
                    fmax = am;
                }
            }
        }
        const crd::u32 ubase = mf.col_ptr[f];
        for (crd::u32 jc = 0; jc < nr; ++jc)
        {
            const crd::u32 cnt = (jc + 1 < npiv) ? (jc + 1) : npiv;
            crd::u32 w = uoff[ubase + jc];
            for (crd::u32 ii = 0; ii < cnt; ++ii)
            {
                const T uv = front.at(ii, jc);
                m_ux[w++] = uv;
                const dense::RealType<T> am = lu2_mag<T>(uv);
                if (am > fmax)
                {
                    fmax = am;
                }
            }
        }
        wgrowth[wk] = fmax;

        // 5e. keep the whole factored buffer as cb[f] (Schur in trailing block); 5f. reset this worker's loc.
        cb_npiv[f] = npiv;
        cb[f] = std::move(front);
        for (crd::u32 i = 0; i < nr; ++i)
        {
            loc_w[rows[i]] = no_loc;
        }
    };

    if (sw <= 1)
    {
        for (crd::u32 f = 0; f < nf; ++f)
        {
            factor_one_front(f, 0, false);
        }
    }
    else
    {
        // Assembly-tree levels: level[f] = 1 + max(level[child]). Children g < f (ascending front order is a
        // postorder) ⇒ a single ascending pass computes levels. Same-level fronts are mutually independent.
        crd::containers::Array<crd::u32> level(m_alloc);
        level.resize(nf);
        crd::u32 nlevels = 0;
        for (crd::u32 f = 0; f < nf; ++f)
        {
            crd::u32 lev = 0;
            for (crd::u32 cc = chld_ptr[f]; cc < chld_ptr[f + 1]; ++cc)
            {
                const crd::u32 lk = level[chld_idx[cc]] + 1;
                lev = lk > lev ? lk : lev;
            }
            level[f] = lev;
            nlevels = (lev + 1 > nlevels) ? lev + 1 : nlevels;
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
        crd::containers::Array<crd::u32> wp(m_alloc);
        wp.resize(nlevels);
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            wp[l] = lvl_ptr[l];
        }
        for (crd::u32 f = 0; f < nf; ++f)
        {
            lvl_list[wp[level[f]]++] = f; // ascending f within a level ⇒ worker-order-independent (the moat)
        }
        // HYBRID parallelism (the Amdahl fix). A level with >= front_par_thresh (≈ sw/2, measured) fronts has
        // enough independent work to SATURATE front-parallel (parallel_for over fronts, each factored serially —
        // all workers busy). A narrower level (1–3 big near-root separators) cannot saturate that way, so its
        // fronts run on the main thread, parallelized WITHIN each (gemm_parallel_auto + parallel TRSM + parallel
        // assembly). Neither path nests a parallel_for in a parallel_for; both are bit-exact ⇒ the moat holds.
        // The threshold ≈ sw/2 (not sw) is measured: routing the k=sw/2..sw-1 levels through front-parallel won
        // wang3 (0.96→1.07–1.11×) and ns3Da (0.95→1.00–1.01×) vs MUMPS @8w. CRD_MF_FRONTPAR_K overrides (dev).
        crd::u32 front_par_thresh = (sw / 2 >= 2) ? sw / 2 : 2;
        if (const char* e = mf_getenv("CRD_MF_FRONTPAR_K"))
        {
            const int v = std::atoi(e);
            if (v >= 2)
            {
                front_par_thresh = static_cast<crd::u32>(v);
            }
        }
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            const crd::u32 lo = lvl_ptr[l];
            const crd::u32 cnt = lvl_ptr[l + 1] - lo;
            if (cnt >= front_par_thresh)
            {
                auto* counter = crd::jobs::parallel_for(cnt, num_workers,
                                                        [&](crd::u32 b, crd::u32 e)
                                                        {
                                                            const crd::u32 wk = crd::jobs::worker_index();
                                                            for (crd::u32 t = b; t < e; ++t)
                                                            {
                                                                factor_one_front(lvl_list[lo + t], wk, false);
                                                            }
                                                        });
                crd::jobs::wait(counter);
                crd::jobs::frame_reset(); // reclaim this level's JobDecls (deep trees issue many levels)
            }
            else
            {
                for (crd::u32 t = 0; t < cnt; ++t)
                {
                    factor_one_front(lvl_list[lo + t], 0, true); // main thread (wk 0) + within-front parallelism
                    // Each narrow front issues parallel_for (assembly + TRSM + trailing GEMM); reclaim its
                    // JobDecls before the next so the per-thread frame arena can't accumulate across the deep
                    // narrow chain. Safe: main thread, and factor_one_front waited all its jobs before returning.
                    crd::jobs::frame_reset();
                }
            }
            // Early-abort the optimistic natural-diagonal attempt the instant element growth blows past the
            // stability bound, so a pathological matrix (e.g. gemat11, whose natural fill explodes) doesn't pay
            // a full doomed factorization before falling back to MC64. Only on the natural attempt (use_mc64
            // false); the MC64 fallback is the last resort and always runs to completion.
            if (!use_mc64)
            {
                dense::RealType<T> g = dense::RealType<T>(0);
                for (crd::u32 w = 0; w < sw; ++w)
                {
                    if (wgrowth[w] > g)
                    {
                        g = wgrowth[w];
                    }
                }
                if (g > static_cast<dense::RealType<T>>(kMfGrowthThreshold) * bnorm)
                {
                    break; // natural growth exceeded the bound → factorize() re-runs with MC64
                }
            }
        }
    }

    // v5f speed-gap profile (CRD_MF_PROFILE, read-only — runs for BOTH the serial and level-scheduled branch):
    // the front-size + flop DISTRIBUTION from the symbolic. Discriminates the serial gap vs MUMPS — a high
    // %-of-flops in SKINNY fronts (npiv<32) means the FUNDAMENTAL (un-amalgamated) supernodes factor at low
    // GFLOP/s ⇒ relaxed amalgamation is the lever; flops concentrated in a few BIG fronts at ~MUMPS GFLOP/s ⇒
    // the microkernel wall ⇒ parity is the ceiling.
    if (mf_getenv("CRD_MF_PROFILE") != nullptr)
    {
        double total_fl = 0.0;
        double skinny_fl = 0.0;
        double big_fl = 0.0;
        crd::u32 max_m = 0;
        crd::u32 max_npiv = 0;
        crd::u32 n_small = 0;
        crd::u32 n_med = 0;
        crd::u32 n_big = 0;
        for (crd::u32 f = 0; f < nf; ++f)
        {
            const double m = static_cast<double>(mf.row_ptr[f + 1] - mf.row_ptr[f]);
            const double p = static_cast<double>(mf.npiv(f));
            const double fl = (2.0 / 3.0) * p * p * p + 2.0 * p * p * (m - p) + 2.0 * p * (m - p) * (m - p);
            total_fl += fl;
            const crd::u32 npv = mf.npiv(f);
            if (npv < 32)
            {
                skinny_fl += fl;
                ++n_small;
            }
            else if (npv < 256)
            {
                ++n_med;
            }
            else
            {
                ++n_big;
            }
            if (npv > max_npiv)
            {
                max_npiv = npv;
                max_m = static_cast<crd::u32>(m);
                big_fl = fl;
            }
        }
        std::fprintf(stderr,
                     "[mf-profile] nf=%u flops=%.3e (skinny npiv<32: %.1f%% of flops over %u fronts) "
                     "npiv-buckets[<32:%u 32-255:%u >=256:%u] biggest front m=%u npiv=%u (%.1f%% of flops)\n",
                     nf, total_fl, 100.0 * skinny_fl / (total_fl + 1e-300), n_small, n_small, n_med, n_big, max_m,
                     max_npiv, 100.0 * big_fl / (total_fl + 1e-300));
    }

    // v5f-(a): finalize partial pivoting. The L row indices were written as PHYSICAL B-rows; remap them ONCE
    // (uniformly) to ELIMINATION indices via invperm (P⁻¹). The diagonal slot holds P[c] ⇒ invperm[P[c]]=c
    // ⇒ L-diagonal-first holds. m_ui is already elimination indices (U rows are always this front's pivots) ⇒
    // untouched. Serial post-pass ⇒ deterministic ⇒ the {1,2,4,8} moat is unaffected.
    if (do_pivot)
    {
        crd::containers::Array<crd::u32> invperm(m_alloc);
        invperm.resize_uninitialized(n);
        for (crd::u32 c = 0; c < n; ++c)
        {
            invperm[m_rowperm[c]] = c; // invperm[P[c]] = c
        }
        for (crd::usize p = 0; p < m_li.size(); ++p)
        {
            m_li[p] = invperm[m_li[p]];
        }
    }

    // Reduce the per-worker growth → the max-element-growth ratio vs ‖B‖ (max is order-independent ⇒ moat-safe).
    dense::RealType<T> mg = dense::RealType<T>(0);
    for (crd::u32 w = 0; w < sw; ++w)
    {
        if (wgrowth[w] > mg)
        {
            mg = wgrowth[w];
        }
    }
    return (bnorm > dense::RealType<T>(0)) ? static_cast<double>(mg) / static_cast<double>(bnorm) : 0.0;
}

template <typename T>
void MultifrontalLU<T>::factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a, crd::u32 num_workers,
                                  dense::RealType<T> pivot_threshold)
{
    CRD_ASSERT_MSG(a.pattern().rows == a.pattern().cols, "MultifrontalLU requires a square matrix");
    m_n = a.pattern().rows;
    m_info = 0;
    m_pivot_threshold = pivot_threshold;
    if (m_n == 0)
    {
        m_lp.clear();
        m_up.clear();
        m_lp.push_back(0U);
        m_up.push_back(0U);
        m_nfront = 0;
        m_lnz = 0;
        m_unz = 0;
        return;
    }
    // ADAPTIVE static pivoting: try the NATURAL diagonal first (faster + better-conditioned on strong-diagonal
    // sim matrices — even fixes saddle-point systems MC64 destabilises); fall back to MC64 only if element
    // growth blows up (genuinely-unsymmetric / circuit). Both paths use STATIC pivoting ⇒ the cross-thread
    // determinism moat holds regardless of which runs. Env overrides: CRD_MF_FORCE_MC64 (always MC64) /
    // CRD_MF_NO_MC64 (natural only, no fallback — dev/experiment).
    const bool force_mc64 = mf_getenv("CRD_MF_FORCE_MC64") != nullptr;
    const bool no_fallback = mf_getenv("CRD_MF_NO_MC64") != nullptr;
    bool use_mc64 = force_mc64;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const double growth = factor_attempt(a, num_workers, use_mc64);
        const bool unstable = !(growth <= kMfGrowthThreshold); // NaN / Inf / over-threshold ⇒ unstable
        if (!unstable || use_mc64 || no_fallback)
        {
            break;
        }
        use_mc64 = true; // natural-diagonal growth blew up → re-factor with MC64 (stable static pivot)
    }
}

template <typename T> bool MultifrontalLU<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return false;
    }
    return static_lu_ir_solve<T>(m_n, m_lp.data(), m_li.data(), m_lx.data(), m_up.data(), m_ui.data(), m_ux.data(),
                                 m_b.pattern().outer_ptr.data(), m_b.pattern().inner_idx.data(),
                                 m_b.values().values.data(), m_scale, rhs, nrhs, m_alloc,
                                 m_rowperm.size() == 0 ? nullptr : m_rowperm.data());
}

// v5f: RAW factor apply (no internal IR) — the mixed-precision IR driver's building block (see the base
// IFactorization::apply_inverse contract). transform_rhs → lu_lu_solve → untransform_solution, no GESP loop.
template <typename T> void MultifrontalLU<T>::apply_inverse(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return; // invalid factor; the driver checks info() before iterating, so rhs is left untouched
    }
    static_lu_apply<T>(m_n, m_lp.data(), m_li.data(), m_lx.data(), m_up.data(), m_ui.data(), m_ux.data(), m_scale,
                       rhs, nrhs, m_alloc, m_rowperm.size() == 0 ? nullptr : m_rowperm.data());
}

template <typename T>
MultifrontalLU<T> factor_multifrontal_lu(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                         crd::memory::IAllocator* alloc, crd::u32 num_workers)
{
    MultifrontalLU<T> lu(alloc);
    lu.factorize(a, num_workers);
    return lu;
}

// Explicit instantiations: f32 / f64 / Complex32 / Complex64 (never defer complex).
template class MultifrontalLU<crd::f32>;
template class MultifrontalLU<crd::f64>;
template class MultifrontalLU<crd::hesap::Complex32>;
template class MultifrontalLU<crd::hesap::Complex64>;
template MultifrontalLU<crd::f32>
factor_multifrontal_lu<crd::f32>(const sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr>&,
                                 crd::memory::IAllocator*, crd::u32);
template MultifrontalLU<crd::f64>
factor_multifrontal_lu<crd::f64>(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>&,
                                 crd::memory::IAllocator*, crd::u32);
template MultifrontalLU<crd::hesap::Complex32> factor_multifrontal_lu<crd::hesap::Complex32>(
    const sparse::SparseMatrix<crd::hesap::Complex32, sparse::SparseFormat::Csr>&, crd::memory::IAllocator*, crd::u32);
template MultifrontalLU<crd::hesap::Complex64> factor_multifrontal_lu<crd::hesap::Complex64>(
    const sparse::SparseMatrix<crd::hesap::Complex64, sparse::SparseFormat::Csr>&, crd::memory::IAllocator*, crd::u32);

template <typename T>
MultifrontalLU<T> factor_multifrontal_lu_pp(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                            crd::memory::IAllocator* alloc, crd::u32 num_workers,
                                            dense::RealType<T> pivot_threshold)
{
    MultifrontalLU<T> lu(alloc);
    lu.factorize(a, num_workers, pivot_threshold);
    return lu;
}
template MultifrontalLU<crd::f32>
factor_multifrontal_lu_pp<crd::f32>(const sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr>&,
                                    crd::memory::IAllocator*, crd::u32, dense::RealType<crd::f32>);
template MultifrontalLU<crd::f64>
factor_multifrontal_lu_pp<crd::f64>(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>&,
                                    crd::memory::IAllocator*, crd::u32, dense::RealType<crd::f64>);

} // namespace crd::hesap::direct
