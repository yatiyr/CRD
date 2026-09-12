#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/block_cg.hpp> // iterative::detail::block_qr (complex-capable thin-QR)
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/sparse/convert.hpp>                   // to_csc
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp> // ParallelSpmvLeastSquaresOp (size-adaptive M·x)
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace crd::hesap::preconditioners
{
// -----------------------------------------------------------------------
// SpaiPreconditioner<T> -- classical (right) Sparse Approximate Inverse.
// Phase 3.1.6 v4i-1.
//
// M ≈ A⁻¹ minimizing ‖A·M − I‖_F. The Frobenius norm decouples by column:
//   min_{m_k} ‖A·m_k − e_k‖₂   for each column k of M, independently.
// Over a prescribed sparsity pattern J_k for m_k, the reduced problem is a
// small dense least-squares: Â·m̂ = ê, where Â = A(I, J_k) (the rows I that
// have a nonzero in any column j∈J_k) and ê = e_k|_I. Solved by Householder
// thin-QR (the complex-capable iterative::detail::block_qr), with an R-diagonal
// floor on the back-substitution (rank-deficient / near-dependent local block).
//
// Pattern J_k:
//   - SpaiPattern::Static   = pattern of column k of A (CSC column k) + the
//     diagonal k (SPAI(0); the standard parallel substrate).
//   - SpaiPattern::Adaptive = Grote-Huckle augmentation: start J_k={k}, compute
//     the residual r = A·m_k − e_k, rank candidate columns by the 1-D
//     minimization gain |⟨A·e_ℓ, r⟩|²/‖A·e_ℓ‖², add the best, re-solve, until
//     ‖r‖₂ ≤ ε or the per-column fill cap is reached. The research-grade variant.
//
// Setup is EMBARRASSINGLY PARALLEL (each column an independent dense QR) -- the
// honest win over the inherently sequential IC/ILU factorization. Per-worker
// scratch is sized once (bounded by the fill cap); a column whose local problem
// would exceed kSpaiLocalMax rows falls back to the diagonal m_k = e_k/A[k,k]
// (the standard SPAI safety net for dense-column matrices; keeps scratch O(1)).
// Columns are assembled into M in fixed column order ⇒ M is BIT-IDENTICAL
// regardless of thread count (the determinism moat at setup).
//
// apply z = M·r is a single spmv (NO triangular solve) on the size-adaptive
// parallel-SELL path (delegated to ParallelSpmvLeastSquaresOp); apply_adjoint
// = Mᴴ·r. This is where SPAI beats IC/ILU on the tri-solve-bound regime
// (sherman3-class) and maps cleanly to a GPU. Classical SPAI's M is NOT
// symmetric even for symmetric A -- use FspaiPreconditioner for the SPD path
// (PCG/MINRES/SYMMLQ). Real + complex (no conjugation in the LS; the adjoint is
// Mᴴ via the stored conjugate-transpose SELL).
// -----------------------------------------------------------------------

enum class SpaiPattern : crd::u8
{
    Static,
    Adaptive
};

namespace detail
{
template <typename T>
[[nodiscard]] inline T spai_conj(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>) { return T{v.re, -v.im}; }
    else { return v; }
}
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> spai_mag(T v) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    if constexpr (crd::hesap::dense::is_complex_v<T>) { return std::sqrt(v.re * v.re + v.im * v.im); }
    else { return v < R(0) ? -v : v; }
}
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> spai_abs2(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re * v.re + v.im * v.im; }
    else { return v * v; }
}

// Per-column local-problem row cap. A column whose I-set exceeds this falls back
// to the diagonal -> per-worker scratch stays O(kSpaiLocalMax * cap_J).
inline constexpr crd::u32 kSpaiLocalMax = 4096U;
// Adaptive: candidate columns added per augmentation step.
inline constexpr crd::u32 kSpaiStepAdd = 5U;
} // namespace detail

template <typename T>
class SpaiPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    SpaiPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, SpaiPattern pattern = SpaiPattern::Static,
                       R epsilon = R(0.4), crd::u32 max_per_col = 0)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_m(build_spai(a, pattern, epsilon, max_per_col, alloc))
        , m_op(m_m, alloc)
        , m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "SpaiPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "SpaiPreconditioner: requires a compressed CSR matrix");
    }

    // z = M·r  (M ≈ A⁻¹; a single size-adaptive parallel spmv, no triangular solve).
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        return m_op.apply(r, z);
    }
    // z = Mᴴ·r.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        return m_op.apply_adjoint(r, z);
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    // nnz of the explicit approximate inverse M (for the bench fill-ratio reporting).
    [[nodiscard]] crd::usize factor_nnz() const noexcept { return m_m.nnz(); }

private:
    // Per-worker scratch (sized once; written, never grown, inside parallel_for).
    struct ColScratch
    {
        crd::containers::Array<crd::u32> jlist;  // J pattern (A column indices)
        crd::containers::Array<crd::u32> ilist;  // I rows (sorted)
        crd::containers::Array<crd::i32> imark;  // global row -> local I index (-1 absent), size n
        crd::containers::Array<crd::i32> jmark;  // column-in-J marker (-1/1), size n
        crd::containers::Array<crd::i32> cmark;  // candidate dedup marker (-1/1), size n
        crd::containers::Array<crd::u32> clist;  // candidate columns (adaptive)
        crd::containers::Array<R>        cprofit; // candidate reductions (adaptive)
        crd::containers::Array<T>        adense; // Â row-major in×jn (block_qr overwrites with Q)
        crd::containers::Array<T>        cm;     // block_qr column-contiguous scratch
        crd::containers::Array<T>        rmat;   // R jn×jn row-major
        crd::containers::Array<T>        cvec;   // Qᴴê then solution m̂
        crd::containers::Array<T>        rhs;    // ê (local I) then residual
        explicit ColScratch(crd::memory::IAllocator* alloc)
            : jlist(alloc), ilist(alloc), imark(alloc), jmark(alloc), cmark(alloc), clist(alloc), cprofit(alloc)
            , adense(alloc), cm(alloc), rmat(alloc), cvec(alloc), rhs(alloc)
        {
        }
    };

    struct Ctx
    {
        const crd::u32* col_ptr; // CSC of A
        const crd::u32* col_row;
        const T*        col_val;
        const crd::u32* row_ptr; // CSR of A (adaptive candidate row scan)
        const crd::u32* row_col;
        ColScratch*     ws; // [num_workers]
        crd::u32*       stage_row;
        T*              stage_val;
        crd::u32*       stage_cnt;
        crd::u32        n;
        crd::u32        cap_J;
        crd::u32        cap_I;
        SpaiPattern     pattern;
        R               epsilon;
    };

    [[nodiscard]] static T diag_of(const Ctx& c, crd::u32 k) noexcept
    {
        for (crd::u32 p = c.col_ptr[k]; p < c.col_ptr[k + 1]; ++p)
        {
            if (c.col_row[p] == k) { return c.col_val[p]; }
        }
        return T{};
    }

    // Solve column k's local LS (with adaptive augmentation) into the staging slot.
    static void process_column(const Ctx& c, crd::u32 k)
    {
        ColScratch&    w  = c.ws[crd::jobs::worker_index()];
        crd::u32*      sr = c.stage_row + static_cast<crd::usize>(k) * c.cap_J;
        T*             sv = c.stage_val + static_cast<crd::usize>(k) * c.cap_J;
        const R        eps_floor = std::sqrt(std::numeric_limits<R>::epsilon());
        const R        smlnum    = std::numeric_limits<R>::min();

        // ---- initial pattern J ----
        crd::u32 jn = 0;
        if (c.pattern == SpaiPattern::Static)
        {
            bool has_diag = false;
            for (crd::u32 p = c.col_ptr[k]; p < c.col_ptr[k + 1] && jn < c.cap_J; ++p)
            {
                const crd::u32 j = c.col_row[p];
                w.jlist[jn++]    = j;
                if (j == k) { has_diag = true; }
            }
            if (!has_diag && jn < c.cap_J)
            {
                w.jlist[jn++] = k;
                std::sort(w.jlist.data(), w.jlist.data() + jn);
            }
        }
        else
        {
            w.jlist[0] = k;
            jn         = 1;
        }
        for (crd::u32 jl = 0; jl < jn; ++jl) { w.jmark[w.jlist[jl]] = 1; }

        crd::u32 in = 0;
        for (;;)
        {
            // ---- build I = sorted union of A's columns in J ----
            in            = 0;
            bool overflow = false;
            for (crd::u32 jl = 0; jl < jn && !overflow; ++jl)
            {
                const crd::u32 j = w.jlist[jl];
                for (crd::u32 p = c.col_ptr[j]; p < c.col_ptr[j + 1]; ++p)
                {
                    const crd::u32 i = c.col_row[p];
                    if (w.imark[i] < 0)
                    {
                        if (in >= c.cap_I) { overflow = true; break; }
                        w.imark[i]   = 0; // present; local index assigned after the sort
                        w.ilist[in++] = i;
                    }
                }
            }
            if (overflow)
            {
                for (crd::u32 t = 0; t < in; ++t) { w.imark[w.ilist[t]] = -1; }
                for (crd::u32 jl = 0; jl < jn; ++jl) { w.jmark[w.jlist[jl]] = -1; }
                // diagonal fallback m_k = e_k / A[k,k]
                T akk = diag_of(c, k);
                if (detail::spai_mag(akk) < smlnum) { akk = T(R(1)); }
                sr[0]            = k;
                sv[0]            = T(R(1)) / akk;
                c.stage_cnt[k]   = 1;
                return;
            }
            std::sort(w.ilist.data(), w.ilist.data() + in);
            for (crd::u32 t = 0; t < in; ++t) { w.imark[w.ilist[t]] = static_cast<crd::i32>(t); }

            // ---- assemble Â (row-major in×jn) + rhs ê ----
            for (crd::usize idx = 0; idx < static_cast<crd::usize>(in) * jn; ++idx) { w.adense[idx] = T{}; }
            for (crd::u32 jl = 0; jl < jn; ++jl)
            {
                const crd::u32 j = w.jlist[jl];
                for (crd::u32 p = c.col_ptr[j]; p < c.col_ptr[j + 1]; ++p)
                {
                    const crd::i32 il                                       = w.imark[c.col_row[p]];
                    w.adense[static_cast<crd::usize>(il) * jn + jl]          = c.col_val[p];
                }
            }
            for (crd::u32 t = 0; t < in; ++t) { w.rhs[t] = (w.ilist[t] == k) ? T(R(1)) : T{}; }

            // ---- thin-QR LS: Â = Q·R, then m̂ = R⁻¹(Qᴴ ê) ----
            crd::hesap::iterative::detail::block_qr<T>(w.adense.data(), in, jn, w.cm.data(), w.rmat.data());
            for (crd::u32 jl = 0; jl < jn; ++jl)
            {
                T acc{};
                for (crd::u32 t = 0; t < in; ++t)
                {
                    acc = acc + detail::spai_conj(w.adense[static_cast<crd::usize>(t) * jn + jl]) * w.rhs[t];
                }
                w.cvec[jl] = acc;
            }
            R dmax = R(0);
            for (crd::u32 d = 0; d < jn; ++d)
            {
                const R m = detail::spai_mag(w.rmat[static_cast<crd::usize>(d) * jn + d]);
                dmax      = m > dmax ? m : dmax;
            }
            const R rfloor = dmax * eps_floor + smlnum;
            for (crd::u32 jj = 0; jj < jn; ++jj)
            {
                const crd::u32 j   = jn - 1 - jj;
                T              acc = w.cvec[j];
                for (crd::u32 kk = j + 1; kk < jn; ++kk)
                {
                    acc = acc - w.rmat[static_cast<crd::usize>(j) * jn + kk] * w.cvec[kk];
                }
                T piv = w.rmat[static_cast<crd::usize>(j) * jn + j];
                if (detail::spai_mag(piv) < rfloor) { piv = T(rfloor); }
                w.cvec[j] = acc / piv;
            }

            // ---- static, capped, or converged adaptive ⇒ done ----
            if (c.pattern == SpaiPattern::Static || jn >= c.cap_J)
            {
                break;
            }

            // ---- adaptive: residual r = A·m_k − e_k on rows I (+ row k if k∉I) ----
            for (crd::u32 t = 0; t < in; ++t) { w.rhs[t] = T{}; }
            for (crd::u32 jl = 0; jl < jn; ++jl)
            {
                const T        mv = w.cvec[jl];
                const crd::u32 j  = w.jlist[jl];
                for (crd::u32 p = c.col_ptr[j]; p < c.col_ptr[j + 1]; ++p)
                {
                    w.rhs[static_cast<crd::usize>(w.imark[c.col_row[p]])] =
                        w.rhs[static_cast<crd::usize>(w.imark[c.col_row[p]])] + c.col_val[p] * mv;
                }
            }
            const bool k_in_i = w.imark[k] >= 0;
            R          rn2    = k_in_i ? R(0) : R(1); // e_k on row k not in I contributes 1
            for (crd::u32 t = 0; t < in; ++t)
            {
                if (w.ilist[t] == k) { w.rhs[t] = w.rhs[t] - T(R(1)); }
                rn2 += detail::spai_abs2(w.rhs[t]);
            }
            if (std::sqrt(rn2) <= c.epsilon) { break; }

            // ---- candidate columns: new cols in CSR rows where r ≠ 0 (+ row k) ----
            crd::u32 cand_n = 0;
            auto     scan_row = [&](crd::u32 i) {
                for (crd::u32 p = c.row_ptr[i]; p < c.row_ptr[i + 1]; ++p)
                {
                    const crd::u32 ell = c.row_col[p];
                    if (w.jmark[ell] < 0 && w.cmark[ell] < 0)
                    {
                        w.cmark[ell]      = 1;
                        w.clist[cand_n++] = ell;
                    }
                }
            };
            for (crd::u32 t = 0; t < in; ++t)
            {
                if (detail::spai_abs2(w.rhs[t]) > R(0)) { scan_row(w.ilist[t]); }
            }
            if (!k_in_i) { scan_row(k); }
            if (cand_n == 0)
            {
                for (crd::u32 t = 0; t < cand_n; ++t) { w.cmark[w.clist[t]] = -1; }
                break;
            }

            // ---- profit ρ²(ℓ) = |⟨A·e_ℓ, r⟩|² / ‖A·e_ℓ‖²  (1-D minimization gain) ----
            for (crd::u32 t = 0; t < cand_n; ++t)
            {
                const crd::u32 ell = w.clist[t];
                T              num{};
                R              den = R(0);
                for (crd::u32 p = c.col_ptr[ell]; p < c.col_ptr[ell + 1]; ++p)
                {
                    const crd::u32 i = c.col_row[p];
                    den += detail::spai_abs2(c.col_val[p]);
                    if (w.imark[i] >= 0)
                    {
                        num = num + detail::spai_conj(c.col_val[p]) * w.rhs[static_cast<crd::usize>(w.imark[i])];
                    }
                    else if (i == k && !k_in_i)
                    {
                        num = num - detail::spai_conj(c.col_val[p]); // r[k] = -1
                    }
                }
                w.cprofit[t] = den > R(0) ? detail::spai_abs2(num) / den : R(0);
            }

            // ---- add the kSpaiStepAdd best candidates (index tiebreak ⇒ deterministic) ----
            crd::u32 added = 0;
            while (added < detail::kSpaiStepAdd && jn < c.cap_J)
            {
                crd::u32 best = cand_n;
                for (crd::u32 t = 0; t < cand_n; ++t)
                {
                    if (w.cmark[w.clist[t]] != 1) { continue; } // already consumed
                    if (best == cand_n || w.cprofit[t] > w.cprofit[best]) { best = t; }
                }
                if (best == cand_n) { break; }
                const crd::u32 ell = w.clist[best];
                w.cmark[ell]       = 2; // consumed
                w.jmark[ell]       = 1;
                w.jlist[jn++]      = ell;
                ++added;
            }
            for (crd::u32 t = 0; t < cand_n; ++t) { w.cmark[w.clist[t]] = -1; } // reset markers
            // reset I markers; the next iteration rebuilds I for the enlarged J
            for (crd::u32 t = 0; t < in; ++t) { w.imark[w.ilist[t]] = -1; }
            if (added == 0) { break; }
        }

        // ---- store m̂ over J into the staging slot ----
        for (crd::u32 jl = 0; jl < jn; ++jl)
        {
            sr[jl] = w.jlist[jl];
            sv[jl] = w.cvec[jl];
        }
        c.stage_cnt[k] = jn;
        for (crd::u32 t = 0; t < in; ++t) { w.imark[w.ilist[t]] = -1; }
        for (crd::u32 jl = 0; jl < jn; ++jl) { w.jmark[w.jlist[jl]] = -1; }
    }

    static Csr build_spai(const Csr& a, SpaiPattern pattern, R epsilon, crd::u32 max_per_col,
                          crd::memory::IAllocator* alloc)
    {
        const crd::u32 n = a.rows();
        crd::hesap::sparse::TripletBuilder<T> tb(alloc, n, n);
        if (n == 0) { return tb.compress(); }

        auto        acsc    = crd::hesap::sparse::to_csc<T>(a, alloc);
        const auto* col_ptr = acsc.pattern().outer_ptr.data();
        const auto* col_row = acsc.pattern().inner_idx.data();
        const T*    col_val = acsc.values().values.data();
        const auto* row_ptr = a.pattern().outer_ptr.data();
        const auto* row_col = a.pattern().inner_idx.data();

        crd::u32 max_col_nnz = 0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            const crd::u32 cnt = col_ptr[j + 1] - col_ptr[j];
            max_col_nnz        = cnt > max_col_nnz ? cnt : max_col_nnz;
        }
        crd::u32 cap_J = (pattern == SpaiPattern::Static)
                             ? (max_col_nnz + 1U)
                             : (max_per_col != 0 ? max_per_col : std::max<crd::u32>(10U, 3U * max_col_nnz));
        if (cap_J > n) { cap_J = n; }
        if (cap_J == 0) { cap_J = 1; }
        crd::u64 cap_i64 = static_cast<crd::u64>(cap_J) * max_col_nnz + 1U;
        const crd::u32 row_lim = detail::kSpaiLocalMax < n ? detail::kSpaiLocalMax : n;
        crd::u32       cap_I   = static_cast<crd::u32>(cap_i64 < row_lim ? cap_i64 : row_lim);
        if (cap_I < 1) { cap_I = 1; }
        if (cap_J > cap_I) { cap_J = cap_I; }

        const crd::u32 workers = crd::jobs::num_workers() == 0 ? 1U : crd::jobs::num_workers();
        crd::containers::Array<ColScratch> ws(alloc);
        ws.reserve(workers);
        for (crd::u32 wi = 0; wi < workers; ++wi)
        {
            ws.push_back(ColScratch(alloc));
            ColScratch& s = ws[wi];
            s.jlist.resize(cap_J);
            s.ilist.resize(cap_I);
            s.imark.resize(n);
            s.jmark.resize(n);
            s.cmark.resize(n);
            s.clist.resize(n);
            s.cprofit.resize(n);
            s.adense.resize(static_cast<crd::usize>(cap_I) * cap_J);
            s.cm.resize(static_cast<crd::usize>(cap_I) * cap_J);
            s.rmat.resize(static_cast<crd::usize>(cap_J) * cap_J);
            s.cvec.resize(cap_J);
            s.rhs.resize(cap_I);
            for (crd::u32 i = 0; i < n; ++i)
            {
                s.imark[i] = -1;
                s.jmark[i] = -1;
                s.cmark[i] = -1;
            }
        }

        crd::containers::Array<crd::u32> stage_row(alloc), stage_cnt(alloc);
        crd::containers::Array<T>        stage_val(alloc);
        stage_row.resize(static_cast<crd::usize>(n) * cap_J);
        stage_val.resize(static_cast<crd::usize>(n) * cap_J);
        stage_cnt.resize(n);

        Ctx ctx{col_ptr, col_row,        col_val,        row_ptr,         row_col, ws.data(),
                stage_row.data(), stage_val.data(), stage_cnt.data(), n,       cap_J,   cap_I,
                pattern,   epsilon};

        const crd::u32 jobs    = workers < n ? workers : n;
        auto*          counter = crd::jobs::parallel_for(n, jobs, [pc = &ctx](crd::u32 b, crd::u32 e) {
            for (crd::u32 k = b; k < e; ++k) { process_column(*pc, k); }
        });
        crd::jobs::wait(counter);
        crd::jobs::frame_reset(); // reclaim the parallel_for JobDecls (Krylov-loop hygiene)

        // Assemble M in fixed column order ⇒ deterministic regardless of thread count.
        for (crd::u32 k = 0; k < n; ++k)
        {
            const crd::u32* sr  = stage_row.data() + static_cast<crd::usize>(k) * cap_J;
            const T*        sv  = stage_val.data() + static_cast<crd::usize>(k) * cap_J;
            const crd::u32  cnt = stage_cnt[k];
            for (crd::u32 t = 0; t < cnt; ++t) { tb.add(sr[t], k, sv[t]); }
        }
        return tb.compress();
    }

    Csr                                                m_m;  // explicit approximate inverse (CSR)
    crd::hesap::sparse::ParallelSpmvLeastSquaresOp<T>  m_op; // size-adaptive M·x / Mᴴ·x
    crd::u32                                           m_n;
};

} // namespace crd::hesap::preconditioners
