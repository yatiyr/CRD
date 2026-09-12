#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/block_cg.hpp> // iterative::detail::block_lu_solve (complex-capable s×s)
#include <crd/hesap/linear_op.hpp>
#include <crd/hesap/preconditioners/spai.hpp> // SpaiPattern + detail::spai_conj/mag/abs2
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
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
// FspaiPreconditioner<T> -- FACTORED Sparse Approximate Inverse for SPD/HPD A.
// Phase 3.1.6 v4i-1 (Kolotilina-Yeremin 1993 / Huckle 2003).
//
// M = L·Lᴴ ≈ A⁻¹ with L lower-triangular and sparse, SYMMETRIC-POSITIVE-DEFINITE
// BY CONSTRUCTION -- the SPD variant classical (right) SPAI cannot give (its M is
// not symmetric even for symmetric A). This is the valid SPAI preconditioner for
// CG / PCG / MINRES / SYMMLQ, exactly as IC(0) is the SPD twin of ILU(0).
//
// Column-decoupled, like classical SPAI. For column k with strict-lower pattern
// P_k ⊆ {k+1,…,n−1}: solve the small SPD system A(P_k,P_k)·y = −A(P_k,k) (dense LU
// with a pivot floor), form d_k = A(k,k) + A(k,P_k)·y (the local Schur
// complement; > 0 for SPD A), and set the k-th column of L:
//   L(k,k) = 1/√d_k ,   L(P_k,k) = y / √d_k .
// (Verified: at full pattern M = L·Lᴴ reproduces A⁻¹ exactly.)
//
// Pattern P_k:
//   - SpaiPattern::Static   = strict-lower nonzeros of column k of A.
//   - SpaiPattern::Adaptive = Huckle augmentation: rank candidate indices j>k by
//     |(A·v)_j|²/A(j,j) (the rank-1 increase in d_k, v = column so far), add the
//     best, re-solve, until the best relative gain ≤ ε² or the fill cap is hit.
//
// Setup is embarrassingly parallel (each column an independent small SPD solve);
// columns assemble into L in fixed column order ⇒ L is BIT-IDENTICAL regardless
// of thread count (the determinism moat at setup). A column whose strict-lower
// pattern exceeds kFspaiLocalMax falls back to diagonal scaling L(k,k)=1/√A(k,k)
// (the dense-column safety net; keeps the s×s buffer O(1)).
//
// apply z = M·r = L·(Lᴴ·r) is two spmv (NO triangular solve) on the size-adaptive
// parallel-SELL path; apply_adjoint == apply (M Hermitian). Real + complex/HPD.
// -----------------------------------------------------------------------

namespace detail
{
inline constexpr crd::u32 kFspaiLocalMax = 256U; // strict-lower pattern cap (s×s scratch bound)
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> spai_real(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>) { return v.re; }
    else { return v; }
}
} // namespace detail

template <typename T>
class FspaiPreconditioner final : public crd::hesap::LinearOp<T>
{
public:
    using R   = crd::hesap::dense::RealType<T>;
    using Csr = crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr>;

    FspaiPreconditioner(const Csr& a, crd::memory::IAllocator* alloc, SpaiPattern pattern = SpaiPattern::Static,
                        R epsilon = R(0.1), crd::u32 max_per_col = 0)
        : crd::hesap::LinearOp<T>(/*has_transpose=*/false, /*has_adjoint=*/true)
        , m_l(build_fspai(a, pattern, epsilon, max_per_col, alloc))
        , m_op(m_l, alloc)
        , m_t(alloc)
        , m_n(a.rows())
    {
        CRD_ASSERT_MSG(a.rows() == a.cols(), "FspaiPreconditioner: matrix must be square");
        CRD_ASSERT_MSG(a.pattern().is_compressed(), "FspaiPreconditioner: requires a compressed CSR matrix");
        m_t.resize(m_n);
    }

    // z = M·r = L·(Lᴴ·r): two spmv, no triangular solve.
    [[nodiscard]] bool apply(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        (void)m_op.apply_adjoint(r, crd::containers::Span<T>{m_t.data(), m_n}); // t = Lᴴ·r
        (void)m_op.apply(crd::containers::ConstSpan<T>{m_t.data(), m_n}, z);    // z = L·t
        return true;
    }
    // M = L·Lᴴ is Hermitian ⇒ Mᴴ = M.
    [[nodiscard]] bool apply_adjoint(crd::containers::ConstSpan<T> r, crd::containers::Span<T> z) const override
    {
        return apply(r, z);
    }

    [[nodiscard]] crd::usize n_rows() const noexcept override { return m_n; }
    [[nodiscard]] crd::usize n_cols() const noexcept override { return m_n; }

    // nnz of the factor L (for the bench fill-ratio reporting).
    [[nodiscard]] crd::usize factor_nnz() const noexcept { return m_l.nnz(); }

private:
    struct ColScratch
    {
        crd::containers::Array<crd::u32> plist;   // P_k (strict-lower rows)
        crd::containers::Array<crd::i32> pmark;   // global row -> local P index (-1), size n
        crd::containers::Array<crd::i32> cmark;   // candidate dedup (-1/1/2), size n (adaptive)
        crd::containers::Array<crd::u32> clist;   // candidate columns (adaptive)
        crd::containers::Array<R>        cprofit; // candidate profits (adaptive)
        crd::containers::Array<T>        vval;    // current column vector v (k + P), size n (adaptive)
        crd::containers::Array<T>        app;     // A(P,P) s×s row-major
        crd::containers::Array<T>        rhs;     // -A(P,k) then y
        crd::containers::Array<T>        akj;     // A(P,k)
        explicit ColScratch(crd::memory::IAllocator* alloc)
            : plist(alloc), pmark(alloc), cmark(alloc), clist(alloc), cprofit(alloc), vval(alloc), app(alloc)
            , rhs(alloc), akj(alloc)
        {
        }
    };

    struct Ctx
    {
        const crd::u32* col_ptr; // CSC of A (column k strict-lower pattern)
        const crd::u32* col_row;
        const T*        col_val;
        const crd::u32* row_ptr; // CSR of A (A(P,P) / A(P,k) / residual rows)
        const crd::u32* row_col;
        const T*        row_val;
        ColScratch*     ws;
        crd::u32*       stage_row;
        T*              stage_val;
        crd::u32*       stage_cnt;
        crd::u32        n;
        crd::u32        cap_J;
        crd::u32        cap_sz;
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

    static void process_column(const Ctx& c, crd::u32 k)
    {
        ColScratch& w  = c.ws[crd::jobs::worker_index()];
        crd::u32*   sr = c.stage_row + static_cast<crd::usize>(k) * c.cap_J;
        T*          sv = c.stage_val + static_cast<crd::usize>(k) * c.cap_J;
        const R     eps_floor = std::sqrt(std::numeric_limits<R>::epsilon());
        const R     smlnum    = std::numeric_limits<R>::min();

        const T akk      = diag_of(c, k);
        const R diagfloor = eps_floor * detail::spai_mag(akk) + smlnum;

        // ---- count strict-lower pattern; dense-column ⇒ diagonal fallback ----
        crd::u32 nlow = 0;
        for (crd::u32 p = c.col_ptr[k]; p < c.col_ptr[k + 1]; ++p)
        {
            if (c.col_row[p] > k) { ++nlow; }
        }
        auto store_diagonal = [&]() {
            R dk = detail::spai_real(akk);
            if (dk < diagfloor) { dk = diagfloor; }
            sr[0]          = k;
            sv[0]          = T(R(1) / std::sqrt(dk));
            c.stage_cnt[k] = 1;
        };
        if (nlow > c.cap_sz)
        {
            store_diagonal();
            return;
        }

        // ---- initial pattern P ----
        crd::u32 sz = 0;
        if (c.pattern == SpaiPattern::Static)
        {
            for (crd::u32 p = c.col_ptr[k]; p < c.col_ptr[k + 1]; ++p)
            {
                const crd::u32 i = c.col_row[p];
                if (i > k)
                {
                    w.plist[sz] = i;
                    w.pmark[i]  = static_cast<crd::i32>(sz);
                    ++sz;
                }
            }
        }
        // adaptive: P starts empty (sz = 0).

        R dk = detail::spai_real(akk);
        for (;;)
        {
            // ---- assemble A(P,P) (s×s row-major), akj = A(P,k), rhs = -akj ----
            for (crd::usize idx = 0; idx < static_cast<crd::usize>(sz) * sz; ++idx) { w.app[idx] = T{}; }
            for (crd::u32 p = 0; p < sz; ++p) { w.akj[p] = T{}; }
            for (crd::u32 p = 0; p < sz; ++p)
            {
                const crd::u32 ip = w.plist[p];
                for (crd::u32 q = c.row_ptr[ip]; q < c.row_ptr[ip + 1]; ++q)
                {
                    const crd::u32 cc = c.row_col[q];
                    if (cc == k) { w.akj[p] = c.row_val[q]; }
                    else if (w.pmark[cc] >= 0)
                    {
                        w.app[static_cast<crd::usize>(p) * sz + static_cast<crd::u32>(w.pmark[cc])] = c.row_val[q];
                    }
                }
                w.rhs[p] = T{} - w.akj[p];
            }

            // ---- solve A(P,P) y = -A(P,k) ----
            if (sz > 0)
            {
                crd::hesap::iterative::detail::block_lu_solve<T>(w.app.data(), sz, w.rhs.data(), 1);
            }

            // ---- d_k = A(k,k) + A(k,P)·y  (A(k,P[p]) = conj(A(P[p],k)) = conj(akj[p])) ----
            T dacc = akk;
            for (crd::u32 p = 0; p < sz; ++p) { dacc = dacc + detail::spai_conj(w.akj[p]) * w.rhs[p]; }
            dk = detail::spai_real(dacc);
            if (dk < diagfloor) { dk = diagfloor; }

            if (c.pattern == SpaiPattern::Static || sz >= c.cap_J) { break; }

            // ---- adaptive augmentation: v = (e_k with y on P); profit_j = |(A·v)_j|²/A(j,j) ----
            w.vval[k] = T(R(1));
            for (crd::u32 p = 0; p < sz; ++p) { w.vval[w.plist[p]] = w.rhs[p]; }

            crd::u32 cand_n   = 0;
            auto     scan_row = [&](crd::u32 i) {
                for (crd::u32 q = c.row_ptr[i]; q < c.row_ptr[i + 1]; ++q)
                {
                    const crd::u32 j = c.row_col[q];
                    if (j > k && w.pmark[j] < 0 && w.cmark[j] < 0)
                    {
                        w.cmark[j]        = 1;
                        w.clist[cand_n++] = j;
                    }
                }
            };
            scan_row(k);
            for (crd::u32 p = 0; p < sz; ++p) { scan_row(w.plist[p]); }

            R best_profit = R(0);
            for (crd::u32 t = 0; t < cand_n; ++t)
            {
                const crd::u32 j = w.clist[t];
                T              av{};
                for (crd::u32 q = c.row_ptr[j]; q < c.row_ptr[j + 1]; ++q)
                {
                    const crd::u32 cc = c.row_col[q];
                    if (cc == k || w.pmark[cc] >= 0) { av = av + c.row_val[q] * w.vval[cc]; }
                }
                const T ajj = diag_of(c, j);
                const R d   = detail::spai_real(ajj);
                w.cprofit[t] = (d > smlnum) ? detail::spai_abs2(av) / d : R(0);
                if (w.cprofit[t] > best_profit) { best_profit = w.cprofit[t]; }
            }

            // reset v for the rows we set (k + P); the candidate markers reset below
            w.vval[k] = T{};
            for (crd::u32 p = 0; p < sz; ++p) { w.vval[w.plist[p]] = T{}; }

            const bool converged = best_profit <= c.epsilon * c.epsilon * dk;
            if (cand_n == 0 || converged)
            {
                for (crd::u32 t = 0; t < cand_n; ++t) { w.cmark[w.clist[t]] = -1; }
                break;
            }

            crd::u32 added = 0;
            while (added < detail::kSpaiStepAdd && sz < c.cap_J)
            {
                crd::u32 best = cand_n;
                for (crd::u32 t = 0; t < cand_n; ++t)
                {
                    if (w.cmark[w.clist[t]] != 1) { continue; }
                    if (best == cand_n || w.cprofit[t] > w.cprofit[best]) { best = t; }
                }
                if (best == cand_n || w.cprofit[best] <= c.epsilon * c.epsilon * dk) { break; }
                const crd::u32 j = w.clist[best];
                w.cmark[j]       = 2; // consumed
                w.pmark[j]       = static_cast<crd::i32>(sz);
                w.plist[sz++]    = j;
                ++added;
            }
            for (crd::u32 t = 0; t < cand_n; ++t) { w.cmark[w.clist[t]] = -1; }
            if (added == 0) { break; }
        }

        // ---- store L column k: (k, 1/√d_k) + (P[p], y[p]/√d_k) ----
        const T inv_sqrt = T(R(1) / std::sqrt(dk));
        sr[0]            = k;
        sv[0]            = inv_sqrt;
        crd::u32 cnt     = 1;
        for (crd::u32 p = 0; p < sz; ++p)
        {
            sr[cnt] = w.plist[p];
            sv[cnt] = w.rhs[p] * inv_sqrt;
            ++cnt;
        }
        c.stage_cnt[k] = cnt;
        for (crd::u32 p = 0; p < sz; ++p) { w.pmark[w.plist[p]] = -1; }
    }

    static Csr build_fspai(const Csr& a, SpaiPattern pattern, R epsilon, crd::u32 max_per_col,
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
        const T*    row_val = a.values().values.data();

        crd::u32 max_low = 0;
        for (crd::u32 j = 0; j < n; ++j)
        {
            crd::u32 low = 0;
            for (crd::u32 p = col_ptr[j]; p < col_ptr[j + 1]; ++p)
            {
                if (col_row[p] > j) { ++low; }
            }
            max_low = low > max_low ? low : max_low;
        }
        crd::u32 cap_J = (pattern == SpaiPattern::Static)
                             ? (max_low + 1U)
                             : (max_per_col != 0 ? max_per_col : std::max<crd::u32>(10U, 3U * max_low + 1U));
        if (cap_J > n) { cap_J = n; }
        if (cap_J == 0) { cap_J = 1; }
        crd::u32 cap_sz = cap_J - 1U; // strict-lower pattern excludes the diagonal
        if (cap_sz > detail::kFspaiLocalMax) { cap_sz = detail::kFspaiLocalMax; }
        if (cap_sz < 1) { cap_sz = 1; }

        const crd::u32 workers = crd::jobs::num_workers() == 0 ? 1U : crd::jobs::num_workers();
        crd::containers::Array<ColScratch> ws(alloc);
        ws.reserve(workers);
        for (crd::u32 wi = 0; wi < workers; ++wi)
        {
            ws.push_back(ColScratch(alloc));
            ColScratch& s = ws[wi];
            s.plist.resize(cap_J);
            s.pmark.resize(n);
            s.cmark.resize(n);
            s.clist.resize(n);
            s.cprofit.resize(n);
            s.vval.resize(n);
            s.app.resize(static_cast<crd::usize>(cap_sz) * cap_sz);
            s.rhs.resize(cap_J);
            s.akj.resize(cap_J);
            for (crd::u32 i = 0; i < n; ++i)
            {
                s.pmark[i] = -1;
                s.cmark[i] = -1;
                s.vval[i]  = T{};
            }
        }

        crd::containers::Array<crd::u32> stage_row(alloc), stage_cnt(alloc);
        crd::containers::Array<T>        stage_val(alloc);
        stage_row.resize(static_cast<crd::usize>(n) * cap_J);
        stage_val.resize(static_cast<crd::usize>(n) * cap_J);
        stage_cnt.resize(n);

        Ctx ctx{col_ptr, col_row, col_val, row_ptr, row_col, row_val, ws.data(),
                stage_row.data(), stage_val.data(), stage_cnt.data(), n, cap_J, cap_sz, pattern, epsilon};

        const crd::u32 jobs    = workers < n ? workers : n;
        auto*          counter = crd::jobs::parallel_for(n, jobs, [pc = &ctx](crd::u32 b, crd::u32 e) {
            for (crd::u32 k = b; k < e; ++k) { process_column(*pc, k); }
        });
        crd::jobs::wait(counter);
        crd::jobs::frame_reset();

        for (crd::u32 k = 0; k < n; ++k)
        {
            const crd::u32* sr  = stage_row.data() + static_cast<crd::usize>(k) * cap_J;
            const T*        sv  = stage_val.data() + static_cast<crd::usize>(k) * cap_J;
            const crd::u32  cnt = stage_cnt[k];
            for (crd::u32 t = 0; t < cnt; ++t) { tb.add(sr[t], k, sv[t]); }
        }
        return tb.compress();
    }

    Csr                                               m_l;  // lower-triangular factor (M = L·Lᴴ)
    crd::hesap::sparse::ParallelSpmvLeastSquaresOp<T> m_op; // size-adaptive L·x / Lᴴ·x
    mutable crd::containers::Array<T>                 m_t;  // intermediate t = Lᴴ·r
    crd::u32                                          m_n;
};

} // namespace crd::hesap::preconditioners
