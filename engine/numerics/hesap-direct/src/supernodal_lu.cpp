#include <crd/containers/sort.hpp>
#include <crd/core/assert.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas3.hpp>
#include <crd/hesap/dense/layout.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/direct/dense_lu_kernels.hpp>
#include <crd/hesap/direct/lu_symbolic.hpp>
#include <crd/hesap/direct/supernodal_lu.hpp>
#include <crd/hesap/ordering/amd.hpp>
#include <crd/hesap/ordering/mc64.hpp>
#include <crd/hesap/ordering/permutation.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

// TEMPORARY (v5b-2c crush profiling): set to 1 to print the serial-factor cmod sub-breakdown.
// Kept dormant (0) between sessions; the relative-indexed-panel rewrite re-enables it.
#define CRD_LU_PROFILE 0 // NOLINT(cppcoreguidelines-macro-usage) preprocessor toggle, not a constant

namespace crd::hesap::direct
{
#if CRD_LU_PROFILE
namespace
{
[[nodiscard]] inline int crd_lu_bucket(crd::u32 v) noexcept
{
    return v <= 1 ? 0 : v <= 2 ? 1 : v <= 4 ? 2 : v <= 8 ? 3 : v <= 16 ? 4 : v <= 32 ? 5 : v <= 64 ? 6 : 7;
}
} // namespace
#endif
namespace
{
// cmod foot-update crossover (crush lever 1): panels with fewer flops than this use the tight
// inline rank-update; larger route to dense::gemm (BLAS-3). Sized so the irregular-unsymmetric
// small supernodes (the common case) skip the gemm machinery's per-call overhead.
constexpr crd::u64 kCmodGemmInlineFlop = 1ULL << 18; // ~64³

// Sentinel for the relative-index map (global row → compact panel position). 0 is a valid position.
constexpr crd::u32 kNoRow = 0xFFFFFFFFU;

// -----------------------------------------------------------------------
// Dense block kernels for the supernode diagonal block (ColMajor, leading dim `ld`).
// nc is modest (supernode width) ⇒ these stay scalar/BLAS-2; the BLAS-3 crush is the
// cmod foot GEMM (`dense::gemm`). Element (row r, col c) of a ColMajor block = d[c*ld + r].
// -----------------------------------------------------------------------

// BLOCKED right-looking no-pivot LU (static + GESP). For nc ≤ diag_block this degenerates to the
// classic unblocked rank-1 form (bit-identical to the prior code); for wide diagonal blocks (the
// af23560 nc≈290 supernodes) it routes the trailing Schur update through the BLAS-3 `dl::gemm`
// (~1.7 → ~40 GFLOP/s). Deterministic per call ⇒ bit-identical across workers (the moat holds).
template <typename T>
void dense_lu_nopivot(T* d, crd::u32 ld, crd::u32 nc, dense::RealType<T> tiny,
                      crd::memory::IAllocator* scratch = nullptr) noexcept
{
    namespace dl = crd::hesap::dense;
    constexpr crd::u32 diag_block = 48;
    for (crd::u32 j0 = 0; j0 < nc; j0 += diag_block)
    {
        const crd::u32 j1 = (j0 + diag_block < nc) ? j0 + diag_block : nc;
        // 1. Factor the panel columns [j0,j1) over the full row height [j0,nc): scale L + rank-1
        //    trailing updates restricted to the PANEL columns (trailing columns ≥ j1 wait for step 3).
        for (crd::u32 k = j0; k < j1; ++k)
        {
            T pivot = d[static_cast<crd::usize>(k) * ld + k];
            const dense::RealType<T> pm = lu2_mag<T>(pivot);
            if (pm < tiny)
            {
                pivot = (pm == dense::RealType<T>(0)) ? lu2_from_real<T>(tiny) : pivot * (tiny / pm);
                d[static_cast<crd::usize>(k) * ld + k] = pivot;
            }
            for (crd::u32 r = k + 1; r < nc; ++r) // L(r,k) = d(r,k)/pivot
            {
                d[static_cast<crd::usize>(k) * ld + r] = d[static_cast<crd::usize>(k) * ld + r] / pivot;
            }
            for (crd::u32 c = k + 1; c < j1; ++c) // rank-1 update of the remaining panel columns
            {
                const T ukc = d[static_cast<crd::usize>(c) * ld + k];
                for (crd::u32 r = k + 1; r < nc; ++r)
                {
                    d[static_cast<crd::usize>(c) * ld + r] =
                        d[static_cast<crd::usize>(c) * ld + r] - d[static_cast<crd::usize>(k) * ld + r] * ukc;
                }
            }
        }
        if (j1 >= nc)
        {
            break;
        }
        const crd::u32 nb = j1 - j0;
        const crd::u32 trail = nc - j1;
        // 2. U block-row: U[j0:j1, j1:nc] = L_diag⁻¹ · A[j0:j1, j1:nc] (unit-lower triangular solve).
        trsm_unit_lower_left<T>(&d[static_cast<crd::usize>(j0) * ld + j0], ld, nb,
                                &d[static_cast<crd::usize>(j1) * ld + j0], ld, trail);
        // 3. Schur update: A[j1:nc, j1:nc] -= L21 · U12 — the BLAS-3 lever for wide diagonal blocks.
        const dl::MatrixView<const T, dl::Layout::ColMajor> l21(&d[static_cast<crd::usize>(j0) * ld + j1], trail,
                                                                nb, ld);
        const dl::MatrixView<const T, dl::Layout::ColMajor> u12(&d[static_cast<crd::usize>(j1) * ld + j0], nb,
                                                                trail, ld);
        dl::MatrixView<T, dl::Layout::ColMajor> c22(&d[static_cast<crd::usize>(j1) * ld + j1], trail, trail, ld);
        dl::gemm<T, dl::Layout::ColMajor>(lu2_from_real<T>(dense::RealType<T>(-1)), l21, u12, lu2_one<T>(), c22,
                                          dl::Trans::None, dl::Trans::None, scratch);
    }
}

// X = X · U11⁻¹ (right solve), U11 upper nc×nc (ColMajor, ldU, diagonal nonzero), X is m×nc
// (ColMajor, ldX). Computes L21 = A21·U11⁻¹ column by column: column j of X depends on
// columns < j (back-substituted across the U columns).
template <typename T>
void trsm_upper_right(const T* u, crd::u32 ldu, crd::u32 nc, T* x, crd::u32 ldx, crd::u32 m) noexcept
{
    for (crd::u32 j = 0; j < nc; ++j)
    {
        T* xj = x + static_cast<crd::usize>(j) * ldx;
        for (crd::u32 k = 0; k < j; ++k) // subtract the already-solved columns
        {
            const T ukj = u[static_cast<crd::usize>(j) * ldu + k];
            const T* xk = x + static_cast<crd::usize>(k) * ldx;
            for (crd::u32 r = 0; r < m; ++r)
            {
                xj[r] = xj[r] - xk[r] * ukj;
            }
        }
        const T ujj = u[static_cast<crd::usize>(j) * ldu + j];
        for (crd::u32 r = 0; r < m; ++r)
        {
            xj[r] = xj[r] / ujj;
        }
    }
}
} // namespace

template <typename T>
void StaticLuScaling<T>::transform_rhs(crd::containers::ConstSpan<T> b, crd::containers::Span<T> c) const
{
    const crd::usize n = d_row.size();
    CRD_ASSERT_MSG(b.size() == n && c.size() == n, "transform_rhs size mismatch");
    // c' = P·(D_r·b): row i of B' is row perm[i] of B (= original row perm[i]).
    for (crd::usize i = 0; i < n; ++i)
    {
        const crd::u32 src = perm[i];
        c[i] = b[src] * static_cast<dense::RealType<T>>(d_row[src]);
    }
}

template <typename T>
void StaticLuScaling<T>::untransform_solution(crd::containers::ConstSpan<T> y, crd::containers::Span<T> x) const
{
    const crd::usize n = d_col.size();
    CRD_ASSERT_MSG(y.size() == n && x.size() == n, "untransform_solution size mismatch");
    // y is the B' solution y'; recover the B solution y[j] = y'[inv_perm[j]], then x[col_match[j]] = D_c·y[j].
    for (crd::u32 j = 0; j < n; ++j)
    {
        const crd::u32 src = col_match[j]; // B's column j came from original col src
        x[src] = y[inv_perm[j]] * static_cast<dense::RealType<T>>(d_col[src]);
    }
}

namespace
{
// B' = P·B·Pᵀ for a CSC matrix WITH values (the post-MC64 fill-reducing symmetric reorder).
// B'(i,j) = B(perm[i], perm[j]); B' column j = B column perm[j] with rows remapped by inv_perm,
// then canonically re-sorted (the symbolic + numeric require ascending row indices per column).
template <typename T>
sparse::SparseMatrix<T, sparse::SparseFormat::Csc> symmetric_permute_csc(
    const sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& b, const ordering::Permutation& p,
    crd::memory::IAllocator* alloc)
{
    const sparse::SparsePattern& bp = b.pattern();
    const crd::u32 n = bp.rows;
    const crd::u32* bptr = bp.outer_ptr.data();
    const crd::u32* bidx = bp.inner_idx.data();
    const T* bval = b.values().values.data();
    const crd::u32* perm = p.perm.data();
    const crd::u32* invp = p.inv_perm.data();

    sparse::SparsePattern op(alloc);
    sparse::SparseValues<T> ov(alloc);
    op.rows = n;
    op.cols = n;
    op.format = sparse::SparseFormat::Csc;
    op.block_size = 1;
    op.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    crd::u32 total = 0;
    crd::u32 max_col = 0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        op.outer_ptr[j] = total;
        const crd::u32 src = perm[j];
        const crd::u32 cn = bptr[src + 1] - bptr[src];
        total += cn;
        max_col = cn > max_col ? cn : max_col;
    }
    op.outer_ptr[n] = total;
    op.inner_idx.resize_uninitialized(total);
    ov.values.resize_uninitialized(total);

    struct RowVal
    {
        crd::u32 row;
        T val;
    };
    crd::containers::Array<RowVal> tmp(alloc);
    tmp.resize(max_col);
    for (crd::u32 j = 0; j < n; ++j)
    {
        const crd::u32 src = perm[j];
        const crd::u32 lo = bptr[src];
        const crd::u32 cn = bptr[src + 1] - lo;
        for (crd::u32 t = 0; t < cn; ++t)
        {
            tmp[t].row = invp[bidx[lo + t]]; // old row → new row under the symmetric perm
            tmp[t].val = bval[lo + t];
        }
        crd::containers::sort(tmp.data(), tmp.data() + cn,
                              [](const RowVal& x, const RowVal& y) { return x.row < y.row; });
        crd::u32 w = op.outer_ptr[j];
        for (crd::u32 t = 0; t < cn; ++t)
        {
            op.inner_idx[w] = tmp[t].row;
            ov.values[w] = tmp[t].val;
            ++w;
        }
    }
    op.recompute_topology_hash();
    return sparse::SparseMatrix<T, sparse::SparseFormat::Csc>(std::move(op), std::move(ov));
}
} // namespace

template <typename T>
StaticLuScaling<T> static_lu_prepare(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                     sparse::SparseMatrix<T, sparse::SparseFormat::Csc>& out_b,
                                     crd::memory::IAllocator* alloc, bool use_mc64)
{
    const sparse::SparsePattern& apat = a.pattern();
    const crd::u32 n = apat.rows;
    CRD_ASSERT_MSG(apat.rows == apat.cols, "static_lu_prepare requires a square matrix");

    StaticLuScaling<T> s(alloc);
    s.d_row.resize(n);
    s.d_col.resize(n);
    s.col_match.resize(n);
    // use_mc64 == false: static pivot on the NATURAL diagonal (identity match + unit scaling), still
    // deterministic ⇒ the moat is preserved (the moat is STATIC pivoting, not MC64; MC64 is only a stability
    // aid). Faster + more accurate on strong-diagonal systems; the caller watches element growth and re-runs
    // with use_mc64 = true if it blows up (genuinely-unsymmetric / circuit matrices).
    if (!use_mc64)
    {
        s.full_rank = true;
        for (crd::u32 i = 0; i < n; ++i)
        {
            s.d_row[i] = 1.0;
            s.d_col[i] = 1.0;
            s.col_match[i] = i;
        }
    }
    else
    {
        // MC64: match the max-weight transversal onto the diagonal + scale toward an I-matrix.
        ordering::Mc64Scaling mc = ordering::mc64_match_and_scale<T>(a, alloc);
        s.full_rank = mc.full_rank;
        for (crd::u32 i = 0; i < n; ++i)
        {
            s.d_row[i] = mc.dr[i];
            s.d_col[i] = mc.dc[i];
            s.col_match[i] =
                mc.colperm[i]; // new col i ← original col colperm[i] (matched entry a[i,colperm[i]] → diagonal)
        }
    }

    // Build B = perm_cols(D_r·A·D_c) in CSC: B's column j is the D_r/D_c-scaled copy of A's
    // column col_match[j] (rows unchanged ⇒ canonical-sorted preserved). The matched entry
    // a[j, col_match[j]] lands at B(j,j).
    auto acsc = sparse::to_csc<T>(a, alloc);
    const sparse::SparsePattern& cpat = acsc.pattern();
    const crd::u32* cp = cpat.outer_ptr.data(); // A's column pointers
    const crd::u32* ci = cpat.inner_idx.data(); // A's row indices
    const T* cv = acsc.values().values.data();  // A's values (column-ordered)

    sparse::SparsePattern bpat(alloc);
    sparse::SparseValues<T> bvals(alloc);
    bpat.rows = n;
    bpat.cols = n;
    bpat.format = sparse::SparseFormat::Csc;
    bpat.block_size = 1;
    bpat.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    crd::u32 total = 0;
    for (crd::u32 j = 0; j < n; ++j)
    {
        bpat.outer_ptr[j] = total;
        const crd::u32 src = s.col_match[j];
        total += cp[src + 1] - cp[src];
    }
    bpat.outer_ptr[n] = total;
    bpat.inner_idx.resize_uninitialized(total);
    bvals.values.resize_uninitialized(total);
    double min_dom = 1e300;
    for (crd::u32 j = 0; j < n; ++j)
    {
        const crd::u32 src = s.col_match[j];
        const auto sc_col = static_cast<dense::RealType<T>>(s.d_col[src]); // column scaling (scalar)
        crd::u32 w = bpat.outer_ptr[j];
        dense::RealType<T> diag = dense::RealType<T>(0);
        dense::RealType<T> colmax = dense::RealType<T>(0);
        for (crd::u32 p = cp[src]; p < cp[src + 1]; ++p)
        {
            const crd::u32 row = ci[p];
            const auto sc = static_cast<dense::RealType<T>>(s.d_row[row]) * sc_col; // D_r[row]·D_c[src]
            const T val = cv[p] * sc;
            bpat.inner_idx[w] = row;
            bvals.values[w] = val;
            ++w;
            const dense::RealType<T> m = lu2_mag<T>(val);
            if (m > colmax)
            {
                colmax = m;
            }
            if (row == j)
            {
                diag = m;
            }
        }
        if (colmax > dense::RealType<T>(0))
        {
            const double ratio = static_cast<double>(diag) / static_cast<double>(colmax);
            if (ratio < min_dom)
            {
                min_dom = ratio;
            }
        }
        else
        {
            min_dom = 0.0; // empty column ⇒ structurally singular
        }
    }
    bpat.recompute_topology_hash();
    s.min_diag_dominance = (n == 0) ? 1.0 : min_dom;
    sparse::SparseMatrix<T, sparse::SparseFormat::Csc> bmat(std::move(bpat), std::move(bvals));

    // Post-MC64 fill-reducing SYMMETRIC reorder (AMD on B+Bᵀ). MC64's matching permutation optimises
    // the diagonal for stability, NOT fill — it can destroy the input's fill-reducing column order
    // (west2021 structural fill 1.96× Eigen). AMD recovers it: B' = P·B·Pᵀ keeps the matched diagonal
    // (symmetric perm ⇒ static pivot still safe) and cuts real flops. Folded into the solve transform.
    // Deterministic (AMD is integer + fixed tie-breaks) ⇒ the cross-thread moat is preserved.
    s.perm.resize(n);
    s.inv_perm.resize(n);
    if (n > 0)
    {
#if CRD_LU_PROFILE
        auto crd_ta = std::chrono::steady_clock::now();
#endif
        const ordering::Permutation pp = ordering::amd_order(bmat.pattern(), alloc);
#if CRD_LU_PROFILE
        std::fprintf(stderr, "[LU-PHASE] n=%u    amd_order=%.2f ms\n", n,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - crd_ta).count());
#endif
        for (crd::u32 i = 0; i < n; ++i)
        {
            s.perm[i] = pp.perm[i];
            s.inv_perm[i] = pp.inv_perm[i];
        }
        out_b = symmetric_permute_csc<T>(bmat, pp, alloc);
    }
    else
    {
        out_b = std::move(bmat);
    }
    return s;
}

namespace
{
// In-place L\ then U\ on z (length n). L unit-lower CSC (lp/li/lx, unit diagonal first per
// column); U upper CSC (up/ui/ux, diagonal last per column). z = RHS in, solution out.
template <typename T>
void lu_lu_solve(crd::u32 n, const crd::u32* lp, const crd::u32* li, const T* lx, const crd::u32* up,
                 const crd::u32* ui, const T* ux, T* z) noexcept
{
    for (crd::u32 j = 0; j < n; ++j) // forward: L\z (unit lower)
    {
        const T zj = z[j];
        for (crd::u32 p = lp[j] + 1; p < lp[j + 1]; ++p) // skip the unit diagonal
        {
            z[li[p]] = z[li[p]] - lx[p] * zj;
        }
    }
    for (crd::u32 jj = 0; jj < n; ++jj) // backward: U\z (upper)
    {
        const crd::u32 j = n - 1 - jj;
        const crd::u32 pdiag = up[j + 1] - 1; // U(j,j) = last entry of column j
        z[j] = z[j] / ux[pdiag];
        const T zj = z[j];
        for (crd::u32 p = up[j]; p < pdiag; ++p) // off-diagonals (rows < j)
        {
            z[ui[p]] = z[ui[p]] - ux[p] * zj;
        }
    }
}
} // namespace

template <typename T>
SupernodalLU<T>::SupernodalLU(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_scale(alloc), m_b(alloc), m_lp(alloc), m_li(alloc), m_lx(alloc), m_up(alloc), m_ui(alloc),
      m_ux(alloc), m_super(alloc)
{
}

template <typename T>
void SupernodalLU<T>::factorize(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a, crd::u32 num_workers)
{
    // num_workers drives the tree-parallel dispatch below (sw / the parallel_for over supernode levels);
    // num_workers <= 1 takes the serial path. The {1,2,4,8} results are bit-identical (the moat).
    CRD_ASSERT_MSG(a.pattern().rows == a.pattern().cols, "SupernodalLU requires a square matrix");
    m_n = a.pattern().rows;
    m_info = 0;

    // v5b-2a: MC64 static-pivot transform → B (CSC, matched on diagonal) + D_r/D_c/perm.
#if CRD_LU_PROFILE
    auto crd_tp0 = std::chrono::steady_clock::now();
#endif
    m_scale = static_lu_prepare<T>(a, m_b, m_alloc);
#if CRD_LU_PROFILE
    auto crd_tp1 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "[LU-PHASE] n=%u  prepare(MC64+AMD+symperm)=%.2f ms\n", m_n,
                 std::chrono::duration<double, std::milli>(crd_tp1 - crd_tp0).count());
#endif

    m_lp.clear();
    m_li.clear();
    m_lx.clear();
    m_up.clear();
    m_ui.clear();
    m_ux.clear();
    m_super.clear();
    const crd::u32 n = m_n;
    if (n == 0)
    {
        m_lp.push_back(0U);
        m_up.push_back(0U);
        m_nsuper = 0;
        m_lnz = 0;
        m_unz = 0;
        return;
    }

    // v5b-2b: exact static-pivot L/U symbolic structure of B.
#if CRD_LU_PROFILE
    auto crd_ts_sym = std::chrono::steady_clock::now();
#endif
    LuSymbolic sym = lu_symbolic(m_b.pattern(), m_alloc);
#if CRD_LU_PROFILE
    std::fprintf(stderr, "[LU-PHASE] n=%u  symbolic=%.2f ms\n", m_n,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - crd_ts_sym).count());
#endif
    m_lp = std::move(sym.lp);
    m_li = std::move(sym.li);
    m_up = std::move(sym.up);
    m_ui = std::move(sym.ui);
    m_super = std::move(sym.super);
    m_nsuper = sym.nsuper;
    m_lnz = sym.lnz;
    m_unz = sym.unz;
    m_lx.resize(static_cast<crd::usize>(m_lnz));
    m_ux.resize(static_cast<crd::usize>(m_unz));

    const crd::u32* bp = m_b.pattern().outer_ptr.data();
    const crd::u32* bi = m_b.pattern().inner_idx.data();
    const T* bx = m_b.values().values.data();

    // GESP perturbation threshold (Demmel-Li): √ε·‖B‖ (max |entry|). MC64 makes B diagonally
    // dominant so a tiny pivot is rare; the perturbation keeps a static factorization going,
    // and iterative refinement (solve) recovers the accuracy. Deterministic ⇒ moat-safe.
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

    // v5b-2c step 2: SUPERNODAL BLAS-3 numeric. Supernodes processed ASCENDING (the fixed
    // deterministic order = the v5b-2d moat reference). Dense ColMajor L panels (scratch) feed a
    // left-looking update: each contributing prior supernode K applies its within-K triangular
    // solve (trsm L_KK⁻¹) + its foot rank-update (the BLAS-3 `dense::gemm` — the crush lever) into
    // a dense work `wk` over R(J) = (contributing-K full ranges) ∪ (J's L row pattern). The
    // diagonal block is then factored no-pivot (STATIC + GESP) and the foot solved (L21 = A21·U11⁻¹).
    // Results scatter into the CSC L/U the verified `lu_lu_solve` reads (the solve is unchanged).
    crd::containers::Array<crd::u32> col_super(m_alloc);
    col_super.resize(n);
    for (crd::u32 s = 0; s < m_nsuper; ++s)
    {
        for (crd::u32 c = m_super[s]; c < m_super[s + 1]; ++c)
        {
            col_super[c] = s;
        }
    }

    // Dense L panels (scratch): supernode s → nr_s × nc_s ColMajor (packed L11+U11 top + L21 foot).
    crd::containers::Array<crd::u32> panelp(m_alloc);
    panelp.resize(static_cast<crd::usize>(m_nsuper) + 1);
    crd::u64 panel_total = 0;
    crd::u32 max_nr = 0;
    crd::u32 max_nc = 0;
    for (crd::u32 s = 0; s < m_nsuper; ++s)
    {
        const crd::u32 c0 = m_super[s];
        const crd::u32 nc = m_super[s + 1] - c0;
        const crd::u32 nr = m_lp[c0 + 1] - m_lp[c0];
        panelp[s] = static_cast<crd::u32>(panel_total);
        panel_total += static_cast<crd::u64>(nr) * nc;
        max_nr = nr > max_nr ? nr : max_nr;
        max_nc = nc > max_nc ? nc : max_nc;
    }
    panelp[m_nsuper] = static_cast<crd::u32>(panel_total);
    crd::containers::Array<T> panel(m_alloc);
    panel.resize(static_cast<crd::usize>(panel_total));

    // RELATIVE-INDEXED COMPACT PANEL (SuperLU-style — the v5b-2c crush structure, replaces the old
    // n × max_nc global-row SPA whose n-stride was cache-hostile: ~54 MB/worker on af23560, polluting
    // the cache so the foot GEMM ran at 15 GFLOP/s in-factor vs 51 in isolation). Each supernode J gets
    // a COMPACT work `W[pnr × nc]` (col-major, ld = pnr), where the panel's touched rows are PACKED:
    //   • U-part rows [0, uoff): each contributor K's FULL column range [k0,k1) laid out consecutively
    //     in ascending (moat) order ⇒ K's U-segment is a CONTIGUOUS knc-row block at offset relmap[k0].
    //   • L-part rows [uoff, uoff+nr): J's L row pattern m_li[rb..) (diagonal block first ⇒ the nc×nc
    //     diagonal LU runs directly on W[uoff..uoff+nc)); foot below.
    // `relmap[global row] → compact position` (sentinel kNoRow when unset; only the pnr entries per
    // supernode are set+reset, O(pnr)). The fill rule guarantees every B row + every contributor-foot
    // row lands in `prow`, so relmap is always set on lookup. Padding contributor ranges to full width
    // = exactly what the old SPA did with structural zeros ⇒ arithmetic order (the moat) is preserved.
    //
    // PRE-PASS: max_pnr = max_s(Σ contributor widths + nr) sizes the per-worker W.
    crd::u32 max_pnr = 0;
    {
        crd::containers::Array<crd::u8> pmark(m_alloc);
        pmark.resize(m_nsuper); // value-init 0
        crd::containers::Array<crd::u32> plist(m_alloc);
        plist.resize(m_nsuper);
        for (crd::u32 s = 0; s < m_nsuper; ++s)
        {
            const crd::u32 c0 = m_super[s];
            const crd::u32 c1 = m_super[s + 1];
            crd::u32 pn = 0;
            crd::u64 uoff = 0;
            for (crd::u32 j = c0; j < c1; ++j)
            {
                for (crd::u32 p = m_up[j]; p < m_up[j + 1]; ++p)
                {
                    const crd::u32 i = m_ui[p];
                    if (i >= c0)
                    {
                        break;
                    }
                    const crd::u32 ks = col_super[i];
                    if (pmark[ks] == 0)
                    {
                        pmark[ks] = 1;
                        plist[pn++] = ks;
                        uoff += m_super[ks + 1] - m_super[ks];
                    }
                }
            }
            for (crd::u32 t = 0; t < pn; ++t)
            {
                pmark[plist[t]] = 0;
            }
            const crd::u64 pnr = uoff + (m_lp[c0 + 1] - m_lp[c0]);
            if (pnr > max_pnr)
            {
                max_pnr = static_cast<crd::u32>(pnr);
            }
        }
    }
    //
    // v5b-2d PARALLELISM: per-worker scratch sized by jobs::num_workers() (worker_index() ranges over
    // it), indexed by worker_index(). Supernodes scheduled by ETREE LEVEL; same-level supernodes have
    // no contributor relation ⇒ independent ⇒ run on `parallel_for`. Each writes only its own panel +
    // L/U columns and reads already-factored (lower-level) contributors ⇒ race-free + bit-identical.
    const crd::u32 sw = (num_workers <= 1) ? 1U : crd::jobs::num_workers();
    crd::containers::Array<crd::u8> super_mark(m_alloc); // sw × nsuper (per-worker contributor dedup)
    super_mark.resize(static_cast<crd::usize>(sw) * m_nsuper);
    crd::containers::Array<crd::u32> ckbuf(m_alloc); // sw × nsuper (per-worker contributor list)
    ckbuf.resize(static_cast<crd::usize>(sw) * m_nsuper);
    crd::containers::Array<T> wkbuf(m_alloc); // sw × max_pnr × max_nc compact panel work (col-major, ld=pnr)
    wkbuf.resize(static_cast<crd::usize>(sw) * max_pnr * max_nc);
    crd::containers::Array<crd::u32> relmap(m_alloc); // sw × n  global row → compact position (kNoRow=unset)
    relmap.resize(static_cast<crd::usize>(sw) * n);
    for (crd::usize i = 0; i < relmap.size(); ++i)
    {
        relmap[i] = kNoRow;
    }
    crd::containers::Array<crd::u32> fposbuf(m_alloc); // sw × max_nr  hoisted foot→compact map (per contributor)
    fposbuf.resize(static_cast<crd::usize>(sw) * max_nr);
    crd::containers::Array<T> ub(m_alloc);
    ub.resize(static_cast<crd::usize>(sw) * max_nr * max_nc);
    // Compact (ld = knc) gather of each contributor's U-segment — the strided ld=n SPA cripples the
    // foot GEMM/TRSV (v5b-2c crush lever 2). knc ≤ max_nc and nc ≤ max_nc ⇒ knc·nc ≤ max_nc².
    crd::containers::Array<T> ucomp(m_alloc);
    ucomp.resize(static_cast<crd::usize>(sw) * max_nc * max_nc);
    // Per-worker bump arena for the foot GEMM's pack buffers. The shared thread_local TLSF fallback
    // churns on the varying per-call pack sizes (~0.8 ms/call ⇒ the af23560 GEMM floor); a reset-per-
    // call LinearAllocator makes the pack alloc a free pointer bump. Bound: a_pack ≤ (max_nr+MR)·max_nc,
    // b_pack ≤ (max_nc+NR)·max_nc; 2× margin + slack covers any block-size choice in dl::gemm.
    crd::usize gemm_arena_bytes = 2 * (static_cast<crd::usize>(max_nr) + 2 * static_cast<crd::usize>(max_nc) + 256) *
                                      static_cast<crd::usize>(max_nc) * sizeof(T) +
                                  (1U << 17);
    gemm_arena_bytes = gemm_arena_bytes < (1U << 25) ? gemm_arena_bytes : (1U << 25); // cap 32 MB/worker
    crd::containers::Array<crd::u8> gemm_scr(m_alloc);
    gemm_scr.resize(static_cast<crd::usize>(sw) * gemm_arena_bytes);

    namespace dl = crd::hesap::dense;
#if CRD_LU_PROFILE
    using crd_clk = std::chrono::steady_clock;
    double t_cmod = 0, t_diag = 0, t_gather = 0, t_scatcsc = 0, t_spain = 0, t_spaclear = 0, t_gemm_only = 0;
    crd::u64 n_upd = 0, n_inline = 0, n_gemm = 0;
    crd::u64 flop_inline = 0, flop_gemm = 0, flop_trsm = 0;
    crd::u64 nc_hist[8] = {}, knc_hist[8] = {}, kfoot_hist[8] = {};
    crd::u64 sum_kfoot = 0;
    auto crd_dt = [](crd_clk::time_point a, crd_clk::time_point b)
    { return std::chrono::duration<double, std::milli>(b - a).count(); };
#endif
    auto factor_one = [&](crd::u32 s, crd::u32 w)
    {
        T* ub_w = ub.data() + static_cast<crd::usize>(w) * max_nr * max_nc;
        T* ucomp_w = ucomp.data() + static_cast<crd::usize>(w) * max_nc * max_nc;
        crd::u8* mark_w = super_mark.data() + static_cast<crd::usize>(w) * m_nsuper;
        crd::u32* ck_w = ckbuf.data() + static_cast<crd::usize>(w) * m_nsuper;
        T* wk_w = wkbuf.data() + static_cast<crd::usize>(w) * max_pnr * max_nc;
        crd::u32* rel_w = relmap.data() + static_cast<crd::usize>(w) * n;
        crd::u32* fpos_w = fposbuf.data() + static_cast<crd::usize>(w) * max_nr;
        const crd::u32 c0 = m_super[s];
        const crd::u32 c1 = m_super[s + 1];
        const crd::u32 nc = c1 - c0;
        const crd::u32 rb = m_lp[c0]; // J's row pattern = m_li[rb .. rb+nr)
        const crd::u32 nr = m_lp[c0 + 1] - rb;
#if CRD_LU_PROFILE
        ++nc_hist[crd_lu_bucket(nc)];
#endif

        // Collect contributing supernodes (U-above entries in J's columns), ascending = the moat order.
        crd::u32 ckn = 0;
        for (crd::u32 j = c0; j < c1; ++j)
        {
            for (crd::u32 p = m_up[j]; p < m_up[j + 1]; ++p)
            {
                const crd::u32 i = m_ui[p];
                if (i >= c0)
                {
                    break; // m_ui ascending: rows < c0 first
                }
                const crd::u32 ks = col_super[i];
                if (mark_w[ks] == 0)
                {
                    mark_w[ks] = 1;
                    ck_w[ckn++] = ks;
                }
            }
        }
        crd::containers::sort(ck_w, ck_w + ckn);
        for (crd::u32 t = 0; t < ckn; ++t)
        {
            mark_w[ck_w[t]] = 0; // reset
        }

#if CRD_LU_PROFILE
        auto crd_ts0 = crd_clk::now();
#endif
        // Build the COMPACT panel layout: U-part = contributor full ranges (ascending) at rows [0,uoff);
        // L-part = m_li at rows [uoff, uoff+nr). relmap[global row] → compact W position.
        crd::u32 uoff = 0;
        for (crd::u32 t = 0; t < ckn; ++t)
        {
            const crd::u32 k0 = m_super[ck_w[t]];
            const crd::u32 kw = m_super[ck_w[t] + 1] - k0;
            for (crd::u32 p = 0; p < kw; ++p)
            {
                rel_w[k0 + p] = uoff + p;
            }
            uoff += kw;
        }
        const crd::u32 pnr = uoff + nr;
        for (crd::u32 i = 0; i < nr; ++i)
        {
            rel_w[m_li[rb + i]] = uoff + i;
        }
        // Zero the compact panel (pnr × nc) — padded rows / fill entries must start at 0.
        for (crd::usize z = 0; z < static_cast<crd::usize>(pnr) * nc; ++z)
        {
            wk_w[z] = T{};
        }
        // Scatter B(:, [c0,c1)) into W via relmap.
        for (crd::u32 j = c0; j < c1; ++j)
        {
            T* col = wk_w + static_cast<crd::usize>(j - c0) * pnr;
            for (crd::u32 p = bp[j]; p < bp[j + 1]; ++p)
            {
                col[rel_w[bi[p]]] = bx[p];
            }
        }
#if CRD_LU_PROFILE
        t_spain += crd_dt(crd_ts0, crd_clk::now());
        auto crd_ts1 = crd_clk::now();
#endif

        // cmod: left-looking supernode-supernode updates (ascending) over the compact panel.
        for (crd::u32 t = 0; t < ckn; ++t)
        {
            const crd::u32 k = ck_w[t];
            const crd::u32 k0 = m_super[k];
            const crd::u32 knc = m_super[k + 1] - k0;
            const crd::u32 knr = m_lp[k0 + 1] - m_lp[k0];
            const crd::u32 kfoot = knr - knc;
            const T* kpanel = &panel[panelp[k]];
            const crd::u32 kfrb = m_lp[k0] + knc; // contributor foot row indices start here
            const crd::u32 uo = rel_w[k0];        // K's U-segment offset in W (contiguous knc-row block)
#if CRD_LU_PROFILE
            ++n_upd;
            ++knc_hist[crd_lu_bucket(knc)];
            ++kfoot_hist[crd_lu_bucket(kfoot)];
            sum_kfoot += kfoot;
            flop_trsm += static_cast<crd::u64>(knc) * (knc > 0 ? knc - 1 : 0) / 2 * nc;
#endif
            // Hoist the foot row → compact-position map ONCE per contributor (shared across all nc
            // panel columns). rel_w[m_li[...]] is a COLD double-indirection; for wide panels (nc≥2)
            // amortizing it over nc columns removes ~(nc−1)·kfoot cold loads (the af23560 scatter
            // floor). Gated nc≥2 so the nc=1 circuit path keeps the lean inline lookup (no regression).
            const bool hoist = (nc >= 2) && (kfoot > 0);
            if (hoist)
            {
                for (crd::u32 fr = 0; fr < kfoot; ++fr)
                {
                    fpos_w[fr] = rel_w[m_li[kfrb + fr]];
                }
            }
            // knc==1 fast path: L_KK = [1] (unit) ⇒ TRSV is identity. Fuse the rank-1 foot update
            // directly into the scattered subtract — no ub buffer, no memset, no function call.
            if (knc == 1)
            {
                if (kfoot == 0)
                {
                    continue;
                }
#if CRD_LU_PROFILE
                ++n_inline;
                flop_inline += static_cast<crd::u64>(kfoot) * nc;
#endif
                const T* lf = kpanel + 1; // foot column of K (length kfoot)
                if (hoist)
                {
                    for (crd::u32 cc = 0; cc < nc; ++cc)
                    {
                        T* col = wk_w + static_cast<crd::usize>(cc) * pnr;
                        const T uval = col[uo];
                        for (crd::u32 fr = 0; fr < kfoot; ++fr)
                        {
                            // Relaxed-amalgamation padding ⇒ kNoRow foot rows (contribution 0) skipped.
                            const crd::u32 pos = fpos_w[fr];
                            if (pos != kNoRow)
                            {
                                col[pos] -= lf[fr] * uval;
                            }
                        }
                    }
                }
                else // nc == 1: lean inline lookup (no hoist amortization).
                {
                    T* col = wk_w; // cc == 0
                    const T uval = col[uo];
                    for (crd::u32 fr = 0; fr < kfoot; ++fr)
                    {
                        const crd::u32 pos = rel_w[m_li[kfrb + fr]];
                        if (pos != kNoRow)
                        {
                            col[pos] -= lf[fr] * uval;
                        }
                    }
                }
                continue;
            }

            // knc >= 2: the U-segment is the contiguous knc-row block W[uo..uo+knc).
            if (kfoot == 0)
            {
                // No foot rows: finalize the U-segment with the TRSV in place (compact, ld=pnr).
                trsm_unit_lower_left<T>(kpanel, knr, knc, &wk_w[uo], pnr, nc);
                continue;
            }
            const crd::u64 cmod_flop = static_cast<crd::u64>(kfoot) * nc * knc;
            if (cmod_flop < kCmodGemmInlineFlop)
            {
                // INLINE path (small panel): TRSV in place on the contiguous U-segment, then a
                // write-on-first rank-knc foot update reading it (ld=pnr, cache-resident).
#if CRD_LU_PROFILE
                ++n_inline;
                flop_inline += cmod_flop;
#endif
                trsm_unit_lower_left<T>(kpanel, knr, knc, &wk_w[uo], pnr, nc);
                const T* lf = kpanel + knc;
                for (crd::u32 jcol = 0; jcol < nc; ++jcol)
                {
                    T* ubj = ub_w + static_cast<crd::usize>(jcol) * kfoot;
                    const T* useg_j = wk_w + static_cast<crd::usize>(jcol) * pnr + uo;
                    const T u0 = useg_j[0]; // pk=0 writes (folds the memset into the first rank-1 update)
                    for (crd::u32 r = 0; r < kfoot; ++r)
                    {
                        ubj[r] = lf[r] * u0;
                    }
                    for (crd::u32 pk = 1; pk < knc; ++pk)
                    {
                        const T upj = useg_j[pk];
                        const T* lcol = lf + static_cast<crd::usize>(pk) * knr;
                        for (crd::u32 r = 0; r < kfoot; ++r)
                        {
                            ubj[r] += lcol[r] * upj;
                        }
                    }
                }
            }
            else
            {
                // GEMM path (wide panel): gather the contiguous U-segment into ucomp (ld=knc) so the
                // BLAS-3 foot GEMM packs a tight operand, then route it through the RowMajor-transpose
                // identity (~15% faster than ColMajor here): C colmajor = Cᵀ rowmajor; C=Lfoot·Useg ⇒
                // Cᵀ = Usegᵀ·Lfootᵀ → gemm<RowMajor>(Usegᵀ, Lfootᵀ) → ubᵀ, same memory.
#if CRD_LU_PROFILE
                ++n_gemm;
                flop_gemm += cmod_flop;
#endif
                for (crd::u32 cc = 0; cc < nc; ++cc)
                {
                    const T* src = wk_w + static_cast<crd::usize>(cc) * pnr + uo;
                    T* dst = ucomp_w + static_cast<crd::usize>(cc) * knc;
                    for (crd::u32 r = 0; r < knc; ++r)
                    {
                        dst[r] = src[r];
                    }
                }
                trsm_unit_lower_left<T>(kpanel, knr, knc, ucomp_w, knc, nc); // compact TRSV
                for (crd::u32 cc = 0; cc < nc; ++cc)                        // writeback solved U-seg to W
                {
                    T* dst = wk_w + static_cast<crd::usize>(cc) * pnr + uo;
                    const T* src = ucomp_w + static_cast<crd::usize>(cc) * knc;
                    for (crd::u32 r = 0; r < knc; ++r)
                    {
                        dst[r] = src[r];
                    }
                }
#if CRD_LU_PROFILE
                auto crd_tg = crd_clk::now();
#endif
                crd::memory::LinearAllocator gemm_arena(
                    gemm_scr.data() + static_cast<crd::usize>(w) * gemm_arena_bytes, gemm_arena_bytes);
                const dl::MatrixView<const T, dl::Layout::RowMajor> useg_t(ucomp_w, nc, knc, knc);
                const dl::MatrixView<const T, dl::Layout::RowMajor> lfoot_t(kpanel + knc, knc, kfoot, knr);
                dl::MatrixView<T, dl::Layout::RowMajor> ub_t(ub_w, nc, kfoot, kfoot);
                dl::gemm<T, dl::Layout::RowMajor>(lu2_one<T>(), useg_t, lfoot_t, T{}, ub_t, dl::Trans::None,
                                                  dl::Trans::None, &gemm_arena);
#if CRD_LU_PROFILE
                t_gemm_only += crd_dt(crd_tg, crd_clk::now());
#endif
            }
            if (hoist)
            {
                for (crd::u32 cc = 0; cc < nc; ++cc)
                {
                    const T* ubc = ub_w + static_cast<crd::usize>(cc) * kfoot;
                    T* col = wk_w + static_cast<crd::usize>(cc) * pnr;
                    for (crd::u32 fr = 0; fr < kfoot; ++fr)
                    {
                        const crd::u32 pos = fpos_w[fr]; // hoisted; kNoRow = padded foot row (ubc=0), skip
                        if (pos != kNoRow)
                        {
                            col[pos] -= ubc[fr];
                        }
                    }
                }
            }
            else // nc == 1
            {
                const T* ubc = ub_w; // cc == 0
                T* col = wk_w;
                for (crd::u32 fr = 0; fr < kfoot; ++fr)
                {
                    const crd::u32 pos = rel_w[m_li[kfrb + fr]];
                    if (pos != kNoRow)
                    {
                        col[pos] -= ubc[fr];
                    }
                }
            }
        }
#if CRD_LU_PROFILE
        t_cmod += crd_dt(crd_ts1, crd_clk::now());
        auto crd_ts2 = crd_clk::now();
#endif

        // Factor the diagonal block (no-pivot, static + GESP) in place — W rows [uoff,uoff+nc), ld=pnr.
        // Blocked for wide nc (BLAS-3 Schur update) — fresh bump arena over the worker's gemm slice.
        crd::memory::LinearAllocator diag_arena(gemm_scr.data() + static_cast<crd::usize>(w) * gemm_arena_bytes,
                                                gemm_arena_bytes);
        dense_lu_nopivot<T>(&wk_w[uoff], pnr, nc, tiny, &diag_arena);
#if CRD_LU_PROFILE
        t_diag += crd_dt(crd_ts2, crd_clk::now());
        auto crd_ts3 = crd_clk::now();
#endif

        // Gather J's panel (m_li order = W L-part rows [uoff,uoff+nr), contiguous), foot L21 = A21·U11⁻¹.
        T* jstore = &panel[panelp[s]];
        for (crd::u32 cc = 0; cc < nc; ++cc)
        {
            const T* src = wk_w + static_cast<crd::usize>(cc) * pnr + uoff;
            T* dst = jstore + static_cast<crd::usize>(cc) * nr;
            for (crd::u32 ga = 0; ga < nr; ++ga)
            {
                dst[ga] = src[ga];
            }
        }
        if (nr > nc)
        {
            trsm_upper_right<T>(jstore, nr, nc, jstore + nc, nr, nr - nc);
        }
#if CRD_LU_PROFILE
        t_gather += crd_dt(crd_ts3, crd_clk::now());
        auto crd_ts4 = crd_clk::now();
#endif

        // Scatter into the CSC factor: U from W (via relmap), L from the gathered panel (unit diag = 1).
        for (crd::u32 j = c0; j < c1; ++j)
        {
            const crd::u32 jj = j - c0;
            const T* col = wk_w + static_cast<crd::usize>(jj) * pnr;
            for (crd::u32 p = m_up[j]; p < m_up[j + 1]; ++p)
            {
                m_ux[p] = col[rel_w[m_ui[p]]];
            }
            const crd::u32 lp0 = m_lp[j];
            const T* jcol = jstore + static_cast<crd::usize>(jj) * nr;
            for (crd::u32 p = lp0; p < m_lp[j + 1]; ++p)
            {
                m_lx[p] = (p == lp0) ? lu2_one<T>() : jcol[jj + (p - lp0)];
            }
        }
#if CRD_LU_PROFILE
        t_scatcsc += crd_dt(crd_ts4, crd_clk::now());
        auto crd_ts5 = crd_clk::now();
#endif

        // Reset relmap (the pnr entries) back to the sentinel for the next supernode.
        for (crd::u32 t = 0; t < ckn; ++t)
        {
            const crd::u32 k0 = m_super[ck_w[t]];
            const crd::u32 kw = m_super[ck_w[t] + 1] - k0;
            for (crd::u32 p = 0; p < kw; ++p)
            {
                rel_w[k0 + p] = kNoRow;
            }
        }
        for (crd::u32 i = 0; i < nr; ++i)
        {
            rel_w[m_li[rb + i]] = kNoRow;
        }
#if CRD_LU_PROFILE
        t_spaclear += crd_dt(crd_ts5, crd_clk::now());
#endif
    };

    if (num_workers <= 1)
    {
        for (crd::u32 s = 0; s < m_nsuper; ++s)
        {
            factor_one(s, 0);
        }
#if CRD_LU_PROFILE
        const double t_total = t_spain + t_cmod + t_diag + t_gather + t_scatcsc + t_spaclear;
        std::fprintf(stderr,
                     "\n[LU-PROFILE] n=%u nsuper=%u  TIMES(ms): spain=%.1f cmod=%.1f diag=%.1f gather=%.1f "
                     "scatcsc=%.1f spaclear=%.1f  total=%.1f\n",
                     n, m_nsuper, t_spain, t_cmod, t_diag, t_gather, t_scatcsc, t_spaclear, t_total);
        std::fprintf(stderr,
                     "[LU-PROFILE] cmod updates=%llu  inline=%llu (%.0f%% flop=%.2fe9)  gemm=%llu (flop=%.2fe9 "
                     "time=%.1fms => %.1f GFLOP/s)  trsm_flop=%.2fe9  avg_kfoot=%.1f\n",
                     static_cast<unsigned long long>(n_upd), static_cast<unsigned long long>(n_inline),
                     n_upd ? 100.0 * static_cast<double>(n_inline) / static_cast<double>(n_upd) : 0.0,
                     static_cast<double>(flop_inline) / 1e9, static_cast<unsigned long long>(n_gemm),
                     static_cast<double>(flop_gemm) / 1e9, t_gemm_only,
                     t_gemm_only > 0 ? 2.0 * static_cast<double>(flop_gemm) / (t_gemm_only * 1e6) : 0.0,
                     static_cast<double>(flop_trsm) / 1e9,
                     n_upd ? static_cast<double>(sum_kfoot) / static_cast<double>(n_upd) : 0.0);
        const char* labels[8] = {"1", "2", "3-4", "5-8", "9-16", "17-32", "33-64", "65+"};
        std::fprintf(stderr, "[LU-PROFILE] nc-hist (supernode width):");
        for (int b = 0; b < 8; ++b)
        {
            std::fprintf(stderr, " %s=%llu", labels[b], static_cast<unsigned long long>(nc_hist[b]));
        }
        std::fprintf(stderr, "\n[LU-PROFILE] knc-hist (contributor width):");
        for (int b = 0; b < 8; ++b)
        {
            std::fprintf(stderr, " %s=%llu", labels[b], static_cast<unsigned long long>(knc_hist[b]));
        }
        std::fprintf(stderr, "\n[LU-PROFILE] kfoot-hist (contributor foot rows):");
        for (int b = 0; b < 8; ++b)
        {
            std::fprintf(stderr, " %s=%llu", labels[b], static_cast<unsigned long long>(kfoot_hist[b]));
        }
        std::fprintf(stderr, "\n");
#endif
    }
    else
    {
        // Supernode etree levels: level(s) = 1 + max(level over contributors). Same-level supernodes
        // are mutually independent ⇒ a `parallel_for` over each level is race-free + bit-identical.
        crd::containers::Array<crd::u32> level(m_alloc);
        level.resize(m_nsuper);
        crd::u32 nlevels = 0;
        for (crd::u32 s = 0; s < m_nsuper; ++s)
        {
            const crd::u32 c0 = m_super[s];
            const crd::u32 c1 = m_super[s + 1];
            crd::u32 lev = 0;
            for (crd::u32 j = c0; j < c1; ++j)
            {
                for (crd::u32 p = m_up[j]; p < m_up[j + 1]; ++p)
                {
                    const crd::u32 i = m_ui[p];
                    if (i >= c0)
                    {
                        break;
                    }
                    const crd::u32 lk = level[col_super[i]] + 1;
                    if (lk > lev)
                    {
                        lev = lk;
                    }
                }
            }
            level[s] = lev;
            if (lev + 1 > nlevels)
            {
                nlevels = lev + 1;
            }
        }
        crd::containers::Array<crd::u32> lvl_ptr(m_alloc);
        lvl_ptr.resize(static_cast<crd::usize>(nlevels) + 1); // value-init 0
        for (crd::u32 s = 0; s < m_nsuper; ++s)
        {
            ++lvl_ptr[level[s] + 1];
        }
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            lvl_ptr[l + 1] += lvl_ptr[l];
        }
        crd::containers::Array<crd::u32> lvl_list(m_alloc);
        lvl_list.resize(m_nsuper);
        crd::containers::Array<crd::u32> wp(m_alloc);
        wp.resize(nlevels);
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            wp[l] = lvl_ptr[l];
        }
        for (crd::u32 s = 0; s < m_nsuper; ++s)
        {
            lvl_list[wp[level[s]]++] = s; // ascending supernode within a level ⇒ deterministic
        }
        for (crd::u32 l = 0; l < nlevels; ++l)
        {
            const crd::u32 lo = lvl_ptr[l];
            const crd::u32 cnt = lvl_ptr[l + 1] - lo;
            auto* counter = crd::jobs::parallel_for(cnt, num_workers,
                                                    [&](crd::u32 b, crd::u32 e)
                                                    {
                                                        const crd::u32 wk = crd::jobs::worker_index();
                                                        for (crd::u32 t = b; t < e; ++t)
                                                        {
                                                            factor_one(lvl_list[lo + t], wk);
                                                        }
                                                    });
            crd::jobs::wait(counter);
            crd::jobs::frame_reset(); // reclaim this level's JobDecls (deep etrees issue many levels)
        }
    }
}

template <typename T> bool SupernodalLU<T>::solve(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return false;
    }
    const crd::u32 n = m_n;
    if (n == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(rhs.size() == static_cast<crd::usize>(n) * nrhs, "SupernodalLU::solve rhs size mismatch");

    const crd::u32* lp = m_lp.data();
    const crd::u32* li = m_li.data();
    const T* lx = m_lx.data();
    const crd::u32* up = m_up.data();
    const crd::u32* ui = m_ui.data();
    const T* ux = m_ux.data();
    const crd::u32* bp = m_b.pattern().outer_ptr.data();
    const crd::u32* bi = m_b.pattern().inner_idx.data();
    const T* bx = m_b.values().values.data();

    crd::containers::Array<T> c(m_alloc);
    crd::containers::Array<T> y(m_alloc);
    crd::containers::Array<T> r(m_alloc);
    c.resize(n);
    y.resize(n);
    r.resize(n);
    const dense::RealType<T> eps = std::numeric_limits<dense::RealType<T>>::epsilon();
    const dense::RealType<T> refine_tol = dense::RealType<T>(64) * eps;
    // Acceptance gate: static pivoting can DIVERGE on indefinite/saddle-point systems (incompressible
    // Navier-Stokes) where IR never recovers (residual O(1) or worse). Below this bound the solution is
    // trustworthy (converged ~1e-11..1e-15 pass; O(1) garbage fails) ⇒ return false rather than silently
    // returning a wrong answer. Such systems need the dynamic-pivot path (v5b-1) or a saddle-point method.
    const dense::RealType<T> accept_tol = std::sqrt(eps); // ~1.5e-8
    bool ok = true;

    for (crd::usize col = 0; col < nrhs; ++col)
    {
        T* b = rhs.data() + col * n;
        m_scale.transform_rhs({b, n}, {c.data(), n}); // c = D_r·b
        for (crd::u32 i = 0; i < n; ++i)
        {
            y[i] = c[i];
        }
        lu_lu_solve<T>(n, lp, li, lx, up, ui, ux, y.data()); // B·y = c (static pivot ⇒ approximate)
        // Iterative refinement on the transformed system B·y = c (Demmel-Li GESP) — drives the
        // TRUE residual to machine precision so the bench compares at a matched residual.
        bool converged = false;
        dense::RealType<T> prev_rn = std::numeric_limits<dense::RealType<T>>::max(); // IR stagnation tracker
        for (crd::u32 it = 0; it < kLuRefineMax; ++it)
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = c[i];
            }
            for (crd::u32 j = 0; j < n; ++j) // r -= B·y (B in CSC)
            {
                const T yj = y[j];
                for (crd::u32 p = bp[j]; p < bp[j + 1]; ++p)
                {
                    r[bi[p]] = r[bi[p]] - bx[p] * yj;
                }
            }
            dense::RealType<T> rn = dense::RealType<T>(0);
            dense::RealType<T> cn = dense::RealType<T>(0);
            for (crd::u32 i = 0; i < n; ++i)
            {
                const dense::RealType<T> rm = lu2_mag<T>(r[i]);
                if (rm > rn)
                {
                    rn = rm;
                }
                const dense::RealType<T> cm = lu2_mag<T>(c[i]);
                if (cm > cn)
                {
                    cn = cm;
                }
            }
            const dense::RealType<T> cnorm = (cn > dense::RealType<T>(0) ? cn : dense::RealType<T>(1));
            if (rn <= refine_tol * cnorm)
            {
                converged = true; // machine-precision on the transformed system B·y = c
                break;
            }
            // Stagnation guard (Demmel-Li GESP): once the residual stops improving by >=2x per step it has
            // hit the static-pivot round-off floor (af23560 flatlines at ~1.4e-14 = refine_tol, so the check
            // above never trips and the loop wastes ~5 solves). Stop, leaving converged=false so the
            // post-loop accept_tol recheck still flags genuine divergence — saddle-point/indefinite systems
            // stall at an O(1) residual and must be rejected, not accepted as "stagnated".
            if (it >= 1U && rn >= static_cast<dense::RealType<T>>(0.5) * prev_rn)
            {
                break;
            }
            prev_rn = rn;
            lu_lu_solve<T>(n, lp, li, lx, up, ui, ux, r.data()); // dy = (LU)\r, in place
            for (crd::u32 i = 0; i < n; ++i)
            {
                y[i] = y[i] + r[i];
            }
        }
        if (!converged) // IR didn't reach refine_tol in kLuRefineMax steps — recheck the FINAL y's residual
        {
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = c[i];
            }
            for (crd::u32 j = 0; j < n; ++j)
            {
                const T yj = y[j];
                for (crd::u32 p = bp[j]; p < bp[j + 1]; ++p)
                {
                    r[bi[p]] = r[bi[p]] - bx[p] * yj;
                }
            }
            dense::RealType<T> rn2 = dense::RealType<T>(0);
            dense::RealType<T> cn2 = dense::RealType<T>(0);
            for (crd::u32 i = 0; i < n; ++i)
            {
                const dense::RealType<T> rm = lu2_mag<T>(r[i]);
                if (rm > rn2)
                {
                    rn2 = rm;
                }
                const dense::RealType<T> cm = lu2_mag<T>(c[i]);
                if (cm > cn2)
                {
                    cn2 = cm;
                }
            }
            if (rn2 > accept_tol * (cn2 > dense::RealType<T>(0) ? cn2 : dense::RealType<T>(1)))
            {
                ok = false; // static factor diverged (indefinite/saddle-point) — solution is unreliable
            }
        }
        m_scale.untransform_solution({y.data(), n}, {b, n}); // x[col_match[j]] = D_c·y[j]
    }
    return ok; // false ⇒ a column failed to converge; the caller must NOT trust the solution
}

template <typename T>
SupernodalLU<T> factor_supernodal_lu(const sparse::SparseMatrix<T, sparse::SparseFormat::Csr>& a,
                                     crd::memory::IAllocator* alloc, crd::u32 num_workers)
{
    SupernodalLU<T> lu(alloc);
    lu.factorize(a, num_workers);
    return lu;
}

// v5f: RAW factor apply (no internal IR) — the mixed-precision IR driver's building block. Same static-pivot
// transform + (the file-local) lu_lu_solve as solve()'s x0 step, MINUS the GESP refinement loop + accept-gate.
// (Uses the anonymous-namespace lu_lu_solve in this TU, not lu_solve.hpp's, to avoid the overload ambiguity.)
template <typename T> void SupernodalLU<T>::apply_inverse(crd::containers::Span<T> rhs, crd::usize nrhs) const
{
    if (m_info != 0)
    {
        return; // invalid factor; the driver checks info() before iterating, so rhs is left untouched
    }
    const crd::u32 n = m_n;
    if (n == 0)
    {
        return;
    }
    crd::containers::Array<T> y(m_alloc);
    y.resize(n);
    for (crd::usize col = 0; col < nrhs; ++col)
    {
        T* b = rhs.data() + col * n;
        m_scale.transform_rhs({b, n}, {y.data(), n});         // y = D_r·b
        lu_lu_solve<T>(n, m_lp.data(), m_li.data(), m_lx.data(), m_up.data(), m_ui.data(), m_ux.data(), y.data());
        m_scale.untransform_solution({y.data(), n}, {b, n});  // x = D_c·y
    }
}

// Explicit instantiations: f32 / f64 / Complex32 / Complex64.
template struct StaticLuScaling<crd::f32>;
template struct StaticLuScaling<crd::f64>;
template struct StaticLuScaling<crd::hesap::Complex32>;
template struct StaticLuScaling<crd::hesap::Complex64>;
template StaticLuScaling<crd::f32>
static_lu_prepare<crd::f32>(const sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr>&,
                            sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csc>&, crd::memory::IAllocator*, bool);
template StaticLuScaling<crd::f64>
static_lu_prepare<crd::f64>(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>&,
                            sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csc>&, crd::memory::IAllocator*, bool);
template StaticLuScaling<crd::hesap::Complex32>
static_lu_prepare<crd::hesap::Complex32>(const sparse::SparseMatrix<crd::hesap::Complex32, sparse::SparseFormat::Csr>&,
                                         sparse::SparseMatrix<crd::hesap::Complex32, sparse::SparseFormat::Csc>&,
                                         crd::memory::IAllocator*, bool);
template StaticLuScaling<crd::hesap::Complex64>
static_lu_prepare<crd::hesap::Complex64>(const sparse::SparseMatrix<crd::hesap::Complex64, sparse::SparseFormat::Csr>&,
                                         sparse::SparseMatrix<crd::hesap::Complex64, sparse::SparseFormat::Csc>&,
                                         crd::memory::IAllocator*, bool);

template class SupernodalLU<crd::f32>;
template class SupernodalLU<crd::f64>;
template class SupernodalLU<crd::hesap::Complex32>;
template class SupernodalLU<crd::hesap::Complex64>;
template SupernodalLU<crd::f32>
factor_supernodal_lu<crd::f32>(const sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr>&,
                               crd::memory::IAllocator*, crd::u32);
template SupernodalLU<crd::f64>
factor_supernodal_lu<crd::f64>(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>&,
                               crd::memory::IAllocator*, crd::u32);
template SupernodalLU<crd::hesap::Complex32> factor_supernodal_lu<crd::hesap::Complex32>(
    const sparse::SparseMatrix<crd::hesap::Complex32, sparse::SparseFormat::Csr>&, crd::memory::IAllocator*, crd::u32);
template SupernodalLU<crd::hesap::Complex64> factor_supernodal_lu<crd::hesap::Complex64>(
    const sparse::SparseMatrix<crd::hesap::Complex64, sparse::SparseFormat::Csr>&, crd::memory::IAllocator*, crd::u32);

} // namespace crd::hesap::direct
