#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/krylov_workspace.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/linear_allocator.hpp>

#include <limits>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// Conjugate Gradient (CG) + Preconditioned CG (PCG). Phase 3.1.6 v4a.
//
// `A` is consumed as a matrix-free LinearOp<T> (sparse, dense, or a stencil).
// `M_inv` (PCG) is an optional LinearOp<T> applying the preconditioner action
// z = M⁻¹ r.
//
// COMPLEX CONTRACT: for complex T, CG/PCG require A (and M) HERMITIAN
// POSITIVE-DEFINITE. The inner product is the Hermitian form ⟨x,y⟩ = xᴴy
// (`dotc`), which is real-positive for an HPD operator in exact arithmetic;
// float arithmetic leaves an O(ε) imaginary part that we deliberately do NOT
// zero (it is bit-deterministic, so zeroing it with a thread-count-dependent
// threshold would be the only way to BREAK determinism). The α = ρ/(pᴴAp)
// complex division is bit-deterministic because both operands come from the
// bit-exact KBN-pairwise `blas1` reductions -- hence the whole solve is
// bit-identical across thread counts {1,2,4,8,16} (the v4 determinism moat).
// -----------------------------------------------------------------------

namespace detail
{
// Hermitian inner product ⟨x,y⟩ = xᴴy (real `dot` for real T, `dotc` for
// complex T). Returns T; for HPD operands ⟨x,x⟩ and ⟨p,Ap⟩ are real-positive.
template <typename T>
[[nodiscard]] inline T krylov_inner(crd::containers::ConstSpan<T> x, crd::containers::ConstSpan<T> y)
{
    using namespace crd::hesap::dense;
    if constexpr (is_complex_v<T>)
    {
        return dotc<RealType<T>>(x, y);
    }
    else
    {
        return dot<T>(x, y);
    }
}

// Magnitude |v| as the real type (fabs for real, hypot for complex). Shared by
// CG/BiCGSTAB breakdown tests.
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> krylov_mag(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return crd::hesap::abs(v);
    }
    else
    {
        return v < T{0} ? -v : v;
    }
}

// LAPACK-style "safe minimum": breakdowns test |scalar| < smlnum rather than
// == 0, so a near-breakdown at scale is reported instead of silently diverging.
template <typename R>
[[nodiscard]] inline R krylov_smlnum() noexcept
{
    return std::numeric_limits<R>::min() / std::numeric_limits<R>::epsilon();
}

// SIGNED real part (not magnitude). The symmetric/Hermitian Lanczos diagonal
// alpha = Re(vᴴ A v) is real and may be NEGATIVE for an indefinite operator;
// taking .re enforces the Hermitian contract (the imaginary part is O(eps)
// rounding, deliberately dropped) without discarding the sign. For real T it is
// the identity.
template <typename T>
[[nodiscard]] inline crd::hesap::dense::RealType<T> krylov_real(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return v.re;
    }
    else
    {
        return v;
    }
}

// Complex conjugate for complex T; identity for real T. Used by the two-sided
// (bi-Lanczos) recurrences (QMR) where conj() appears on scalar coefficients and
// must collapse to a no-op in the real instantiation.
template <typename T>
[[nodiscard]] inline T krylov_conj(T v) noexcept
{
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        return crd::hesap::conj(v);
    }
    else
    {
        return v;
    }
}

// Plane-rotation result shared by the Krylov solvers that carry a Givens QR
// (GMRES, QMR): G·[a;b] = [r;0] with c real (cosine), s in T (sine), c²+|s|²=1.
template <typename T>
struct GivensRot
{
    crd::hesap::dense::RealType<T> c; // real cosine
    T                              s; // sine (complex for complex T)
    T                              r; // rotated diagonal
};

// Bump arena with parent fallback: small per-iteration dense ops (the GCROT
// truncation eig, the block-CG gemm pack buffers) allocate from a pre-sized
// per-workspace LinearAllocator (reset each iteration ⇒ no per-iteration
// malloc/free), falling back to the parent allocator if ever exhausted
// (correctness never depends on the arena sizing — only how often the fallback
// triggers).
class FallbackArena final : public crd::memory::IAllocator
{
public:
    FallbackArena(crd::memory::LinearAllocator* arena, crd::memory::IAllocator* parent) noexcept
        : m_arena(arena), m_parent(parent)
    {
    }
    void* allocate(crd::usize size, crd::usize alignment = crd::memory::kDefaultAlignment) override
    {
        void* p = m_arena->allocate(size, alignment);
        return p != nullptr ? p : m_parent->allocate(size, alignment);
    }
    void deallocate(void* p) noexcept override
    {
        if (p != nullptr && !m_arena->owns(p))
        {
            m_parent->deallocate(p); // arena blocks reclaimed in bulk via reset()
        }
    }
    [[nodiscard]] bool owns(const void* p) const noexcept override { return m_arena->owns(p) || m_parent->owns(p); }

private:
    crd::memory::LinearAllocator* m_arena;
    crd::memory::IAllocator*      m_parent;
};

// Unified (P)CG. `m_inv == nullptr` ⇒ plain CG (z aliases r). `x` is the
// in/out solution (caller seeds the initial guess; pass a zeroed span for x₀=0).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> cg_impl(const crd::hesap::LinearOp<T>&       a,
                                                        const crd::hesap::LinearOp<T>*       m_inv,
                                                        crd::containers::ConstSpan<T>        b,
                                                        crd::containers::Span<T>             x,
                                                        const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                        KrylovWorkspace<T>&                  ws,
                                                        crd::memory::IAllocator*             result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "cg: operator must be square");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "cg: span/workspace size mismatch");

    const auto r  = ws.r.span();
    const auto p  = ws.p.span();
    const auto ap = ws.ap.span();
    // z = preconditioned residual; aliases r when unpreconditioned.
    const auto z = (m_inv != nullptr) ? ws.z.span() : ws.r.span();

    // r = b - A·x
    (void)a.apply(x, ap); // ap = A·x (reused as scratch before the loop)
    for (crd::usize i = 0; i < n; ++i)
    {
        r[i] = b[i] - ap[i];
    }

    const R res0 = nrm2<T>(r);
    R       res  = res0;
    if (opts.record_residuals)
    {
        result.residual_history.push_back(res0);
    }
    if (is_converged<R>(res, res0, opts) || n == 0)
    {
        result.converged           = true;
        result.reason              = StopReason::Converged;
        result.final_residual_norm = res;
        return result;
    }

    if (m_inv != nullptr)
    {
        (void)m_inv->apply(r, z); // z = M⁻¹ r
    }
    dense::copy<T>(z, p); // p = z
    T rho = detail::krylov_inner<T>(r, z);

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        (void)a.apply(p, ap); // ap = A·p
        const T pap = detail::krylov_inner<T>(p, ap);
        if (detail::krylov_mag<T>(pap) < smlnum)
        {
            result.iterations          = k - 1;
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        const T alpha = rho / pap;

        dense::axpy<T>(alpha, p, x);   // x += α·p
        dense::axpy<T>(-alpha, ap, r); // r -= α·A·p

        res                = nrm2<T>(r);
        result.iterations  = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        if (is_converged<R>(res, res0, opts))
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            return result;
        }

        if (m_inv != nullptr)
        {
            (void)m_inv->apply(r, z); // z = M⁻¹ r
        }
        const T rho_new = detail::krylov_inner<T>(r, z);
        if (detail::krylov_mag<T>(rho) < smlnum)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            return result;
        }
        const T beta = rho_new / rho;

        // p = z + β·p
        dense::scal<T>(beta, p);
        dense::axpy<T>(T(1), z, p);
        rho = rho_new;
    }

    result.reason              = StopReason::MaxIterations;
    result.final_residual_norm = res;
    return result;
}
} // namespace detail

// ---- Public CG -------------------------------------------------------

// Unpreconditioned CG (A SPD / HPD). `x` is in/out (seed the initial guess).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> cg(const crd::hesap::LinearOp<T>&                          a,
                                                   crd::containers::ConstSpan<T>                          b,
                                                   crd::containers::Span<T>                               x,
                                                   const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                   KrylovWorkspace<T>&                                    ws,
                                                   crd::memory::IAllocator*                               result_alloc)
{
    return detail::cg_impl<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

// Preconditioned CG. `m_inv` applies z = M⁻¹ r (M SPD/HPD).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> pcg(const crd::hesap::LinearOp<T>&                          a,
                                                    const crd::hesap::LinearOp<T>&                         m_inv,
                                                    crd::containers::ConstSpan<T>                          b,
                                                    crd::containers::Span<T>                               x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    KrylovWorkspace<T>&                                    ws,
                                                    crd::memory::IAllocator* result_alloc)
{
    return detail::cg_impl<T>(a, &m_inv, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
