#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/direct/factorization.hpp>
#include <crd/hesap/direct/multifrontal_ldlt.hpp>
#include <crd/hesap/direct/multifrontal_lu.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/sparse_pattern.hpp>
#include <crd/hesap/sparse/sparse_values.hpp>
#include <crd/hesap/sparse/spmv.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace crd::hesap::direct
{
// =======================================================================
// v5f — MIXED-PRECISION ITERATIVE REFINEMENT (HPL-AI / Carson-Higham 2018).
//
// Factor A in LOW precision (f32 — ~2x the dense-front kernel rate + ~1/2 the factor memory) then recover
// FULL working-precision (f64) accuracy by iterative refinement: the residual r = b - A*x is formed in f64
// against the original matrix, the correction d = (low factor)^-1 * r is computed by applying the CHEAP f32
// factor, and x += d until the backward error converges. Standard two-precision LU-IR (Demmel-Higham): it
// reaches f64 accuracy when kappa(A)*u_low <~ 1 (kappa <~ 1e7 for f32); beyond that it stalls and the
// backward-error-ACCEPT guard FLAGS it (info()/return false) rather than returning silent garbage.
//
// Family-agnostic: `IterativeRefinedSolve<TWork, TLow, LowFactor>` holds an owned f64 copy of A (for the
// residual) + the concrete low-precision factor BY VALUE, and IS an `IFactorization<TWork>` (drops into the
// solve-many / CLI / bench surfaces unchanged). It drives the factor's RAW `apply_inverse` (NO inner IR) so
// the inner low-precision IR's stagnation/accept-gate cannot spuriously fail the outer working-precision IR
// on the ill-conditioned systems mixed-precision targets (the v5f-a `apply_inverse` virtual + the per-family
// raw-apply overrides exist for exactly this).
//
// Determinism MOAT: the IR loop is a deterministic, fixed-order composition (fixed-order `spmv` residual +
// the low factor's bit-identical-across-workers `apply_inverse` + the bit-identical low FACTOR) ⇒ the
// iteration count and every x are worker-count-independent ⇒ factor-in-f32 AND IR-solve are bit-identical
// across {1,2,4,8} (v5f-c). The differentiator no gold-standard mixed-precision solver carries.
// =======================================================================

// Iterative-refinement loop options. Tolerances are derived from eps<TWork> in the driver.
struct MixedRefineOptions
{
    crd::u32 max_iters = 20; // hard cap; the stall guard normally stops in 2-4 iterations
};

// Deep-copy a COMPRESSED CSR `SparseMatrix<TSrc>` into a `SparseMatrix<TDst>`, casting values TSrc -> TDst.
// TDst == TSrc clones (the owned working-precision residual matrix); TDst=f32 / TSrc=f64 downcasts (the
// low-precision factor input). The structural pattern is identical; only the value type changes.
template <typename TDst, typename TSrc>
[[nodiscard]] sparse::SparseMatrix<TDst, sparse::SparseFormat::Csr>
csr_cast_copy(crd::memory::IAllocator* alloc, const sparse::SparseMatrix<TSrc, sparse::SparseFormat::Csr>& src)
{
    const sparse::SparsePattern& sp = src.pattern();
    CRD_ASSERT_MSG(sp.is_compressed(), "csr_cast_copy requires a compressed CSR matrix (call make_compressed)");
    sparse::SparsePattern pat(alloc);
    pat.rows = sp.rows;
    pat.cols = sp.cols;
    pat.format = sparse::SparseFormat::Csr;
    pat.block_size = sp.block_size;
    pat.outer_ptr.resize(sp.outer_ptr.size());
    for (crd::usize i = 0; i < sp.outer_ptr.size(); ++i)
    {
        pat.outer_ptr[i] = sp.outer_ptr[i];
    }
    pat.inner_idx.resize(sp.inner_idx.size());
    for (crd::usize i = 0; i < sp.inner_idx.size(); ++i)
    {
        pat.inner_idx[i] = sp.inner_idx[i];
    }
    pat.recompute_topology_hash();
    sparse::SparseValues<TDst> vals(alloc);
    vals.values.resize(src.values().values.size());
    for (crd::usize i = 0; i < src.values().values.size(); ++i)
    {
        vals.values[i] = static_cast<TDst>(src.values().values[i]);
    }
    return sparse::SparseMatrix<TDst, sparse::SparseFormat::Csr>(std::move(pat), std::move(vals));
}

template <typename TWork, typename TLow, typename LowFactor>
class IterativeRefinedSolve final : public IFactorization<TWork>
{
    static_assert(std::is_same_v<TWork, crd::f32> || std::is_same_v<TWork, crd::f64>,
                  "IterativeRefinedSolve: v5f real working precision (complex mixed-precision is a follow-on)");

public:
    // `a_work` = the owned working-precision (f64) matrix for the residual (full CSR). `low` = the already-
    // built low-precision (f32) factor. Both are moved in (move-only, idiomatic — no UniquePtr in crd).
    IterativeRefinedSolve(crd::memory::IAllocator* alloc,
                          sparse::SparseMatrix<TWork, sparse::SparseFormat::Csr>&& a_work, LowFactor&& low,
                          MixedRefineOptions opts = {}) noexcept
        : m_alloc(alloc), m_a(std::move(a_work)), m_low(std::move(low)), m_opts(opts)
    {
        m_n = static_cast<crd::usize>(m_a.rows());
    }

    [[nodiscard]] bool solve(crd::containers::Span<TWork> rhs, crd::usize nrhs) const override;
    using IFactorization<TWork>::solve; // un-hide the single-RHS convenience overload

    [[nodiscard]] crd::usize n() const noexcept override { return m_n; }
    [[nodiscard]] crd::u64 factor_nnz() const noexcept override { return m_low.factor_nnz(); }
    [[nodiscard]] crd::usize info() const noexcept override { return m_low.info(); }

    // IR iterations applied on the LAST solved column (diagnostics + the load-bearing tests: a real mixed
    // solve refines beyond the bare f32 solution, so this is >= 1 unless the RHS was already at tolerance).
    [[nodiscard]] crd::u32 last_iters() const noexcept { return m_last_iters; }

    // The wrapped low-precision factor (const). For the determinism moat test: its L/U/D values are
    // bit-identical across worker counts, and so — composed with the deterministic IR — is the solution.
    [[nodiscard]] const LowFactor& low_factor() const noexcept { return m_low; }

private:
    crd::memory::IAllocator* m_alloc = nullptr;
    sparse::SparseMatrix<TWork, sparse::SparseFormat::Csr> m_a; // owned working-precision matrix (residual)
    LowFactor m_low;                                           // the cheap low-precision factor (by value)
    MixedRefineOptions m_opts;
    crd::usize m_n = 0;
    mutable crd::u32 m_last_iters = 0; // diagnostic only (last-column iteration count)
};

template <typename TWork, typename TLow, typename LowFactor>
bool IterativeRefinedSolve<TWork, TLow, LowFactor>::solve(crd::containers::Span<TWork> rhs, crd::usize nrhs) const
{
    if (m_low.info() != 0)
    {
        return false; // the low-precision factor is singular ⇒ no trustworthy solve
    }
    const crd::u32 n = static_cast<crd::u32>(m_n);
    if (n == 0)
    {
        return true;
    }
    CRD_ASSERT_MSG(rhs.size() == static_cast<crd::usize>(n) * nrhs, "IterativeRefinedSolve::solve rhs size mismatch");

    using R = dense::RealType<TWork>;
    const R eps = std::numeric_limits<R>::epsilon();
    const R refine_tol = static_cast<R>(64) * eps; // machine-precision backward-error target
    const R accept_tol = std::sqrt(eps);           // below this, the (possibly stalled) solution is trustworthy
    auto mag = [](TWork v) -> R { return v < TWork{0} ? -v : v; };

    crd::containers::Array<TWork> x(m_alloc);
    crd::containers::Array<TWork> r(m_alloc);
    crd::containers::Array<TWork> bsave(m_alloc);
    crd::containers::Array<TLow> rl(m_alloc); // low-precision residual / correction (in place under apply_inverse)
    x.resize(n);
    r.resize(n);
    bsave.resize(n);
    rl.resize(n);

    bool all_ok = true;
    for (crd::usize col = 0; col < nrhs; ++col)
    {
        TWork* b = rhs.data() + col * n;
        R bnorm = R{0};
        for (crd::u32 i = 0; i < n; ++i)
        {
            bsave[i] = b[i];
            x[i] = TWork{0};
            const R m = mag(b[i]);
            if (m > bnorm)
            {
                bnorm = m;
            }
        }
        const R bn = (bnorm > R{0} ? bnorm : R{1});

        bool converged = false;
        R prev_rn = std::numeric_limits<R>::max(); // stall tracker
        crd::u32 it = 0;
        for (; it < m_opts.max_iters; ++it)
        {
            // r = b - A*x  (working precision, fixed-order spmv ⇒ moat-safe). x == 0 at it==0 ⇒ r == b.
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = bsave[i];
            }
            sparse::spmv<TWork>(static_cast<TWork>(-1), m_a, sparse::Trans::None, {x.data(), n}, static_cast<TWork>(1),
                                {r.data(), n});
            R rn = R{0};
            for (crd::u32 i = 0; i < n; ++i)
            {
                const R m = mag(r[i]);
                if (m > rn)
                {
                    rn = m;
                }
            }
            if (rn <= refine_tol * bn)
            {
                converged = true; // machine-precision backward error
                break;
            }
            // Stall guard (Demmel-Li): once the residual stops improving by >=2x per step it has hit the
            // low-precision round-off floor. Stop; the post-loop accept_tol recheck still flags divergence.
            if (it >= 1U && rn >= static_cast<R>(0.5) * prev_rn)
            {
                break;
            }
            prev_rn = rn;
            // d = (low factor)^-1 * r : cast r -> f32, RAW apply (no inner IR), accumulate the correction at f64.
            for (crd::u32 i = 0; i < n; ++i)
            {
                rl[i] = static_cast<TLow>(r[i]);
            }
            m_low.apply_inverse({rl.data(), n}, 1);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] += static_cast<TWork>(rl[i]);
            }
        }
        for (crd::u32 i = 0; i < n; ++i)
        {
            b[i] = x[i];
        }
        m_last_iters = it;
        if (!converged)
        {
            // Backward-error ACCEPT guard: recheck the final residual. Above accept_tol the low-precision
            // factor diverged for this RHS (kappa(A)*u_low too large) ⇒ flag, never return silent garbage.
            for (crd::u32 i = 0; i < n; ++i)
            {
                r[i] = bsave[i];
            }
            sparse::spmv<TWork>(static_cast<TWork>(-1), m_a, sparse::Trans::None, {x.data(), n}, static_cast<TWork>(1),
                                {r.data(), n});
            R rn2 = R{0};
            for (crd::u32 i = 0; i < n; ++i)
            {
                const R m = mag(r[i]);
                if (m > rn2)
                {
                    rn2 = m;
                }
            }
            if (rn2 > accept_tol * bn)
            {
                all_ok = false;
            }
        }
    }
    return all_ok;
}

// v5f-a — factor a general square unsymmetric A (CSR) in LOW precision (f32) and wrap it in a working-
// precision (f64) iterative-refinement solver. The f32 copy of A is a factor-time transient (factor_*
// copies its input); the owned f64 copy backs the residual. Returns an `IFactorization<f64>`.
[[nodiscard]] inline IterativeRefinedSolve<crd::f64, crd::f32, MultifrontalLU<crd::f32>>
factor_mixed_lu(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr>& a, crd::memory::IAllocator* alloc,
                crd::u32 num_workers = 1, MixedRefineOptions opts = {})
{
    sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr> a_low = csr_cast_copy<crd::f32>(alloc, a);
    MultifrontalLU<crd::f32> low = factor_multifrontal_lu<crd::f32>(a_low, alloc, num_workers);
    sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr> a_work = csr_cast_copy<crd::f64>(alloc, a);
    return IterativeRefinedSolve<crd::f64, crd::f32, MultifrontalLU<crd::f32>>(alloc, std::move(a_work),
                                                                              std::move(low), opts);
}

// CSC twin of csr_cast_copy — the low-precision LDLᵀ factor reads its symmetric input as lower-triangle CSC.
template <typename TDst, typename TSrc>
[[nodiscard]] sparse::SparseMatrix<TDst, sparse::SparseFormat::Csc>
csc_cast_copy(crd::memory::IAllocator* alloc, const sparse::SparseMatrix<TSrc, sparse::SparseFormat::Csc>& src)
{
    const sparse::SparsePattern& sp = src.pattern();
    CRD_ASSERT_MSG(sp.is_compressed(), "csc_cast_copy requires a compressed CSC matrix (call make_compressed)");
    sparse::SparsePattern pat(alloc);
    pat.rows = sp.rows;
    pat.cols = sp.cols;
    pat.format = sparse::SparseFormat::Csc;
    pat.block_size = sp.block_size;
    pat.outer_ptr.resize(sp.outer_ptr.size());
    for (crd::usize i = 0; i < sp.outer_ptr.size(); ++i)
    {
        pat.outer_ptr[i] = sp.outer_ptr[i];
    }
    pat.inner_idx.resize(sp.inner_idx.size());
    for (crd::usize i = 0; i < sp.inner_idx.size(); ++i)
    {
        pat.inner_idx[i] = sp.inner_idx[i];
    }
    pat.recompute_topology_hash();
    sparse::SparseValues<TDst> vals(alloc);
    vals.values.resize(src.values().values.size());
    for (crd::usize i = 0; i < src.values().values.size(); ++i)
    {
        vals.values[i] = static_cast<TDst>(src.values().values[i]);
    }
    return sparse::SparseMatrix<TDst, sparse::SparseFormat::Csc>(std::move(pat), std::move(vals));
}

// Expand a SYMMETRIC matrix's LOWER triangle (row >= col, read from CSC `a`; upper ignored) into a FULL
// symmetric CSR (both triangles), values cast to TWork. The mixed-precision driver's residual is a full-CSR
// `spmv`, so the symmetric (Cholesky / LDLᵀ) families feed their lower-read systems through here once at
// factor time. Emits ASCENDING-per-row CSR directly (no sort): a row's lower cols arrive as the CSC columns
// ascend, then its diagonal, then its upper mirror as the column == row is scanned.
template <typename TWork, typename TSrc>
[[nodiscard]] sparse::SparseMatrix<TWork, sparse::SparseFormat::Csr>
symmetric_lower_to_full_csr(crd::memory::IAllocator* alloc,
                            const sparse::SparseMatrix<TSrc, sparse::SparseFormat::Csc>& a)
{
    const sparse::SparsePattern& sp = a.pattern();
    CRD_ASSERT_MSG(sp.is_compressed(), "symmetric_lower_to_full_csr requires a compressed CSC matrix");
    CRD_ASSERT_MSG(sp.rows == sp.cols, "symmetric_lower_to_full_csr requires a square matrix");
    const crd::u32 n = sp.rows;
    const crd::u32* op = sp.outer_ptr.data();
    const crd::u32* ii = sp.inner_idx.data();
    const TSrc* vv = a.values().values.data();

    crd::containers::Array<crd::u32> cnt(alloc);
    cnt.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        cnt[i] = 0;
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 p = op[c]; p < op[c + 1]; ++p)
        {
            const crd::u32 i = ii[p];
            if (i < c)
            {
                continue; // lower triangle only (row >= col)
            }
            ++cnt[i];
            if (i > c)
            {
                ++cnt[c]; // the symmetric upper mirror (c, i)
            }
        }
    }
    sparse::SparsePattern pat(alloc);
    pat.rows = n;
    pat.cols = n;
    pat.format = sparse::SparseFormat::Csr;
    pat.block_size = 1;
    pat.outer_ptr.resize(static_cast<crd::usize>(n) + 1);
    crd::u32 acc = 0;
    for (crd::u32 r = 0; r < n; ++r)
    {
        pat.outer_ptr[r] = acc;
        acc += cnt[r];
    }
    pat.outer_ptr[n] = acc;
    pat.inner_idx.resize(acc);
    sparse::SparseValues<TWork> vals(alloc);
    vals.values.resize(acc);

    crd::containers::Array<crd::u32> cur(alloc);
    cur.resize(n);
    for (crd::u32 r = 0; r < n; ++r)
    {
        cur[r] = pat.outer_ptr[r];
    }
    for (crd::u32 c = 0; c < n; ++c)
    {
        for (crd::u32 p = op[c]; p < op[c + 1]; ++p)
        {
            const crd::u32 i = ii[p];
            if (i < c)
            {
                continue;
            }
            const TWork v = static_cast<TWork>(vv[p]);
            const crd::u32 pos_lower = cur[i]++;
            pat.inner_idx[pos_lower] = c; // (i, c)
            vals.values[pos_lower] = v;
            if (i > c)
            {
                const crd::u32 pos_upper = cur[c]++;
                pat.inner_idx[pos_upper] = i; // (c, i) mirror
                vals.values[pos_upper] = v;
            }
        }
    }
    pat.recompute_topology_hash();
    return sparse::SparseMatrix<TWork, sparse::SparseFormat::Csr>(std::move(pat), std::move(vals));
}

// v5f-b — factor a symmetric INDEFINITE matrix A (lower-triangle CSC) in LOW precision (f32 multifrontal
// LDLᵀ) and wrap it in a working-precision (f64) iterative-refinement solver.
[[nodiscard]] inline IterativeRefinedSolve<crd::f64, crd::f32, MultifrontalLDLT<crd::f32>>
factor_mixed_ldlt(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csc>& a, crd::memory::IAllocator* alloc,
                  crd::u32 num_workers = 1, MixedRefineOptions opts = {})
{
    sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csc> a_low = csc_cast_copy<crd::f32>(alloc, a);
    MultifrontalLDLT<crd::f32> low = factor_multifrontal_ldlt<crd::f32>(a_low, alloc, num_workers);
    sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr> a_work = symmetric_lower_to_full_csr<crd::f64>(alloc, a);
    return IterativeRefinedSolve<crd::f64, crd::f32, MultifrontalLDLT<crd::f32>>(alloc, std::move(a_work),
                                                                                std::move(low), opts);
}

// v5f-b — factor a symmetric SPD matrix A (lower-triangle CSC) in LOW precision (f32 supernodal Cholesky) and
// wrap it in a working-precision (f64) iterative-refinement solver. Cholesky's solve() is already a raw
// triangular apply (no internal IR), so it uses the base `apply_inverse` default.
[[nodiscard]] inline IterativeRefinedSolve<crd::f64, crd::f32, SupernodalCholesky<crd::f32>>
factor_mixed_cholesky(const sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csc>& a,
                      crd::memory::IAllocator* alloc, crd::u32 num_workers = 1, MixedRefineOptions opts = {})
{
    sparse::SparseMatrix<crd::f64, sparse::SparseFormat::Csr> a_work = symmetric_lower_to_full_csr<crd::f64>(alloc, a);
    sparse::SparseMatrix<crd::f32, sparse::SparseFormat::Csr> a32 = csr_cast_copy<crd::f32>(alloc, a_work);
    SupernodalCholesky<crd::f32> low = factor_supernodal_cholesky<crd::f32>(
        a32.pattern(), {a32.values().values.data(), a32.values().values.size()}, alloc, kSupernodeRelax, num_workers);
    return IterativeRefinedSolve<crd::f64, crd::f32, SupernodalCholesky<crd::f32>>(alloc, std::move(a_work),
                                                                                  std::move(low), opts);
}

} // namespace crd::hesap::direct
