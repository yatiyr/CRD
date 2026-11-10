#pragma once

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/blas1.hpp>
#include <crd/hesap/dense/real_type.hpp>
#include <crd/hesap/dense/vector.hpp>
#include <crd/hesap/iterative/cg.hpp> // detail::krylov_inner / krylov_mag / krylov_real / krylov_smlnum
#include <crd/hesap/iterative/iterative_result.hpp>
#include <crd/hesap/iterative/stopping.hpp>
#include <crd/hesap/linear_op.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>

namespace crd::hesap::iterative
{
// -----------------------------------------------------------------------
// QMR -- Quasi-Minimal Residual (Freund-Nachtigal 1991). Phase 3.1.6 v4d-2a.
//
// SHORT-recurrence Krylov solver for GENERAL (nonsymmetric / non-Hermitian) A.
// Built on the two-sided (bi-)Lanczos process, so it consumes BOTH A·x and
// Aᴴ·y every iteration (operator must provide apply_adjoint, e.g. a square
// ParallelSpmvLeastSquaresOp). Unlike BiCGSTAB its residual is quasi-minimized
// (smooth, monotone-ish) rather than oscillating; unlike GMRES it has O(1)
// storage. Eigen ships no QMR -- breadth + the determinism moat.
//
// INNER-PRODUCT CONVENTION: all dots are CONJUGATED (Hermitian `dotc`), matching
// the single module policy (BiCGSTAB/LSQR): the Lanczos process runs on Aᴴ with
// conjugated dots, NOT the classic Aᵀ bilinear form of the 1991 paper.
// The left shadow start is c = r₀ (deterministic; cᴴr₀ = ‖r₀‖² ≠ 0).
//
// PRECONDITIONING: optional RIGHT preconditioner N = m_inv (solve A·N⁻¹·y = b,
// x = N⁻¹·y). Right-side keeps the quasi-residual in the TRUE-residual norm
// (matches BiCGSTAB). The bi-Lanczos adjoint sequence applies N⁻ᴴ, so a
// preconditioner used here MUST provide apply_adjoint (asserted). Unpreconditioned
// (m_inv == nullptr) is bit-identical (N = I: Nvₖ aliases vₖ, p aliases s, no
// untransform). The solution is untransformed once after the loop: x ← N⁻¹·x.
//
// Recurrence: the coupled two-term form of Freund-Nachtigal 1991 (bi-Lanczos
// tridiagonal reduced by successive plane rotations, direction wₖ from the last two
// directions, x += ζₖ·wₖ), with the Hermitian-conjugate rotations of
// detail::sym_givens (Golub & Van Loan section 5.1.8; Bindel et al. 2002). The
// reported rNorm = |ζ̄|·√τ is the QUASI-residual (an upper bound on ‖b−Ax‖);
// tests verify the true residual separately. Determinism: reductions are
// KBN-pairwise, the only parallel steps are the operator's spmv/adjoint-spmv
// (bit-exact across threads) ⇒ bit-identical solve across thread counts.
// -----------------------------------------------------------------------

namespace detail
{
// Plane rotation that annihilates the real, non-negative Lanczos subdiagonal
// entry b against a (T). Returns (c real, s in T, r in T) with
// [c sᴴ; -s c]·[a; b] = [r; 0], c² + |s|² = 1. This is the reflector-free
// Givens form of Golub & Van Loan, "Matrix Computations", section 5.1.8, with the
// complex phase handling of Bindel, Demmel, Kahan and Marques 2002 (the cosine
// stays real, the phase of a moves into s). The real-case sign convention (r >= 0,
// the cosine carries the sign of a, c = 1 when a = b = 0) is Paige and Saunders'
// symmetric rotation. It is the rotation that
// Freund-Nachtigal 1991 apply to the tridiagonal QR of QMR. The larger of |a|, |b|
// is the divisor of the tangent so the root never overflows. Reuses the shared
// detail::GivensRot<T> (a DIFFERENT convention from gmres_givens, hence its own fn).
template <typename T>
[[nodiscard]] inline GivensRot<T> sym_givens(T a, crd::hesap::dense::RealType<T> b) noexcept
{
    using R = crd::hesap::dense::RealType<T>;
    GivensRot<T> rot{};
    if constexpr (crd::hesap::dense::is_complex_v<T>)
    {
        const R mag_a = detail::krylov_mag<T>(a);
        const R mag_b = b < R(0) ? -b : b; // b is a norm (>= 0); guard anyway
        if (mag_b == R(0))
        {
            // nothing below the diagonal: identity rotation
            rot.c = R(1);
            rot.s = T{};
            rot.r = a;
            return rot;
        }
        if (mag_a == R(0))
        {
            // a vanishes: swap the two rows
            rot.c = R(0);
            rot.s = T(R(1));
            rot.r = T(mag_b);
            return rot;
        }
        // phase of a as a unit complex number; 1/phase carries it into s
        if (mag_b > mag_a)
        {
            const R tan_t     = mag_a / mag_b;
            const R cosine    = R(1) / std::sqrt(R(1) + tan_t * tan_t);
            const T inv_phase = T(1) / (a / mag_a);
            rot.s             = T(cosine) * crd::hesap::conj(inv_phase);
            rot.c             = cosine * tan_t;
            rot.r             = T(mag_b) / crd::hesap::conj(rot.s);
        }
        else
        {
            const R tan_t     = mag_b / mag_a;
            const R cosine    = R(1) / std::sqrt(R(1) + tan_t * tan_t);
            const T inv_phase = T(1) / (a / mag_a);
            rot.s             = T(cosine * tan_t) * crd::hesap::conj(inv_phase);
            rot.c             = cosine;
            rot.r             = a / T(cosine);
        }
    }
    else
    {
        const R abs_a = a < R(0) ? -a : a;
        const R abs_b = b < R(0) ? -b : b;
        const R dir_a = a > R(0) ? R(1) : (a < R(0) ? R(-1) : R(0)); // sign of a
        const R dir_b = b > R(0) ? R(1) : (b < R(0) ? R(-1) : R(0)); // sign of b
        if (b == R(0))
        {
            // a is already the pivot; keep its sign in c so that r = |a|
            rot.c = a == R(0) ? R(1) : dir_a;
            rot.s = R(0);
            rot.r = abs_a;
        }
        else if (a == R(0))
        {
            rot.c = R(0);
            rot.s = dir_b;
            rot.r = abs_b;
        }
        else if (abs_b > abs_a)
        {
            const R tan_t = a / b;
            rot.s         = dir_b / std::sqrt(R(1) + tan_t * tan_t);
            rot.c         = rot.s * tan_t;
            rot.r         = b / rot.s;
        }
        else
        {
            const R tan_t = b / a;
            rot.c         = dir_a / std::sqrt(R(1) + tan_t * tan_t);
            rot.s         = rot.c * tan_t;
            rot.r         = a / rot.c;
        }
    }
    return rot;
}
} // namespace detail

template <typename T>
struct QmrWorkspace
{
    // Bi-Lanczos vectors v*/u*, matvec scratch q/s, three w-recurrence directions,
    // and (preconditioned only) the right-precond v-side Nv + adjoint-side p.
    crd::hesap::dense::Vector<T> v_old, v, u_old, u, q, s, w0, w1, w2, nv, p;

    QmrWorkspace(crd::memory::IAllocator* alloc, crd::usize n)
        : v_old(alloc, n), v(alloc, n), u_old(alloc, n), u(alloc, n), q(alloc, n), s(alloc, n), w0(alloc, n),
          w1(alloc, n), w2(alloc, n), nv(alloc, n), p(alloc, n)
    {
    }

    [[nodiscard]] crd::usize size() const noexcept { return v.size(); }
};

// QMR with optional RIGHT preconditioner N = m_inv (nullptr ⇒ plain QMR).
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> qmr(const crd::hesap::LinearOp<T>&  a,
                                                    const crd::hesap::LinearOp<T>*  m_inv,
                                                    crd::containers::ConstSpan<T>   b,
                                                    crd::containers::Span<T>        x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    QmrWorkspace<T>&                ws,
                                                    crd::memory::IAllocator*        result_alloc)
{
    using namespace crd::hesap::dense;
    using R = RealType<T>;

    IterativeResult<R> result(result_alloc);
    const R            smlnum = detail::krylov_smlnum<R>();
    const crd::usize   n      = a.n_rows();
    const bool         prec   = (m_inv != nullptr);
    CRD_ASSERT_MSG(a.n_rows() == a.n_cols(), "qmr: operator must be square");
    CRD_ASSERT_MSG(a.has_adjoint(), "qmr: operator must provide apply_adjoint (Aᴴ)");
    CRD_ASSERT_MSG(!prec || m_inv->has_adjoint(), "qmr: right preconditioner must provide apply_adjoint (N⁻ᴴ)");
    CRD_ASSERT_MSG(b.size() == n && x.size() == n && ws.size() == n, "qmr: span/workspace size mismatch");

    const auto v_old = ws.v_old.span();
    const auto v     = ws.v.span();
    const auto u_old = ws.u_old.span();
    const auto u     = ws.u.span();
    const auto q     = ws.q.span();
    const auto s     = ws.s.span();
    auto       w0    = ws.w0.span();
    auto       w1    = ws.w1.span();
    auto       w2    = ws.w2.span();
    const auto nv    = prec ? ws.nv.span() : v; // Nvₖ = N⁻¹vₖ (aliases vₖ when N = I)
    const auto p     = prec ? ws.p.span() : s;  // p = N⁻ᴴ·(Aᴴuₖ) (aliases s when N = I)

    // Residual r₀ = b - A·x (A·x lands in q as scratch). It seeds the right
    // Lanczos vector; the left shadow vector is the same r₀.
    (void)a.apply(x, q);
    for (crd::usize i = 0; i < n; ++i)
    {
        v[i]     = b[i] - q[i];
        v_old[i] = T{};
        u_old[i] = T{};
        w0[i]    = T{};
        w1[i]    = T{};
    }
    const R res0 = nrm2<T>(v);
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

    // Freund-Nachtigal 1991 scale the start pair so that uᴴv = 1 with
    // |β₁| = |γ₁|: here ⟨c, r₀⟩ = ‖r₀‖², β₁ = sqrt|⟨c, r₀⟩|, γ₁ = ⟨c, r₀⟩/β₁.
    const T start_dot = detail::krylov_inner<T>(v, v);
    if (detail::krylov_mag<T>(start_dot) < smlnum)
    {
        result.reason              = StopReason::Breakdown; // ⟨c, r₀⟩ = 0
        result.final_residual_norm = res;
        return result;
    }
    R beta  = std::sqrt(detail::krylov_mag<T>(start_dot));
    T gamma = start_dot / T(beta);
    // v₁ = r₀/β₁, u₁ = r₀/conj(γ₁)
    dense::copy<T>(v, u);
    dense::scal<T>(T(R(1) / beta), v);
    dense::scal<T>(T(1) / detail::krylov_conj<T>(gamma), u);

    // Rotations of the two previous steps (cosines real, sines in T), the
    // right-hand-side entry ζ̄ of the transformed quasi-residual problem, and
    // τ = Σ‖vⱼ‖² which scales it into the quasi-residual norm.
    R prev2_cos = R(0), prev1_cos = R(0);
    T prev2_sin = T{}, prev1_sin = T{};
    T zeta_bar  = T(beta);
    R tau       = detail::krylov_real<T>(detail::krylov_inner<T>(v, v));

    for (crd::usize k = 1; k <= opts.max_iter; ++k)
    {
        // ---- two-sided Lanczos step ----
        // right side: Nvₖ = N⁻¹vₖ, q = A·Nvₖ; left side: s = Aᴴuₖ, p = N⁻ᴴs
        if (prec)
        {
            (void)m_inv->apply(v, nv);
        }
        (void)a.apply(nv, q);
        (void)a.apply_adjoint(u, s);
        if (prec)
        {
            (void)m_inv->apply_adjoint(s, p);
        }

        // three-term orthogonalisation: remove the older pair, then the current one
        dense::axpy<T>(-gamma, v_old, q);
        dense::axpy<T>(T(-beta), u_old, p);
        const T alpha = detail::krylov_inner<T>(u, q);
        dense::axpy<T>(-alpha, v, q);
        dense::axpy<T>(-detail::krylov_conj<T>(alpha), u, p);

        // next off-diagonal pair from ⟨p, q⟩; a vanishing product is a Lanczos breakdown
        const T    cross             = detail::krylov_inner<T>(p, q);
        const R    beta_n            = std::sqrt(detail::krylov_mag<T>(cross));
        const bool lanczos_breakdown = beta_n < smlnum;
        T          gamma_n{};
        if (!lanczos_breakdown)
        {
            gamma_n = cross / T(beta_n);
        }

        // ---- column k of the tridiagonal, pushed through the earlier rotations ----
        // entries above the diagonal: eps (row k-2) and lam (row k-1); bottom: diag_bar
        T eps{}, lam{}, diag_bar = alpha;
        if (k >= 2)
        {
            T raw_upper = gamma; // untouched entry in row k-1
            if (k >= 3)
            {
                eps       = prev2_sin * gamma;
                raw_upper = -T(prev2_cos) * gamma;
            }
            lam       = T(prev1_cos) * raw_upper + prev1_sin * alpha;
            diag_bar  = detail::krylov_conj<T>(prev1_sin) * raw_upper - T(prev1_cos) * alpha;
            prev2_sin = prev1_sin;
            prev2_cos = prev1_cos;
        }

        // the new rotation removes β from below the diagonal
        const detail::GivensRot<T> rot        = detail::sym_givens<T>(diag_bar, beta_n);
        const T                    diag       = rot.r;
        const T                    zeta       = T(rot.c) * zeta_bar;
        const T                    zeta_bar_n = detail::krylov_conj<T>(rot.s) * zeta_bar;
        prev1_sin                             = rot.s;
        prev1_cos                             = rot.c;

        if (detail::krylov_mag<T>(diag) < smlnum)
        {
            result.iterations          = k;
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            break;
        }

        // ---- search direction wₖ = (vₖ - λ·wₖ₋₁ - ε·wₖ₋₂)/diag and the update x += ζ·wₖ ----
        dense::copy<T>(v, w2);
        if (k >= 2)
        {
            dense::axpy<T>(-lam, w1, w2);
        }
        if (k >= 3)
        {
            dense::axpy<T>(-eps, w0, w2);
        }
        dense::scal<T>(T(1) / diag, w2);
        dense::axpy<T>(zeta, w2, x);

        // slide the direction window: (w₀, w₁, w₂) <- (w₁, w₂, old w₀)
        {
            auto recycled = w0;
            w0            = w1;
            w1            = w2;
            w2            = recycled;
        }

        // ---- shift the Lanczos pair and normalise the new one ----
        dense::copy<T>(v, v_old);
        dense::copy<T>(u, u_old);
        if (!lanczos_breakdown)
        {
            dense::copy<T>(q, v);
            dense::scal<T>(T(R(1) / beta_n), v);
            dense::copy<T>(p, u);
            dense::scal<T>(T(1) / detail::krylov_conj<T>(gamma_n), u);
        }

        // ---- quasi-residual |ζ̄ₖ₊₁|·sqrt(τₖ₊₁) and the stopping tests ----
        tau += detail::krylov_real<T>(detail::krylov_inner<T>(v, v));
        res               = detail::krylov_mag<T>(zeta_bar_n) * std::sqrt(tau);
        result.iterations = k;
        if (opts.record_residuals)
        {
            result.residual_history.push_back(res);
        }
        if (is_converged<R>(res, res0, opts))
        {
            result.converged           = true;
            result.reason              = StopReason::Converged;
            result.final_residual_norm = res;
            break;
        }
        if (lanczos_breakdown)
        {
            result.reason              = StopReason::Breakdown;
            result.final_residual_norm = res;
            break;
        }
        if (k == opts.max_iter)
        {
            result.reason              = StopReason::MaxIterations;
            result.final_residual_norm = res;
        }

        zeta_bar = zeta_bar_n;
        beta     = beta_n;
        gamma    = gamma_n;
    }

    // Right-precond untransform: x ← N⁻¹·x (q is free scratch after the loop).
    if (prec)
    {
        (void)m_inv->apply(x, q);
        dense::copy<T>(q, x);
    }
    return result;
}

// Plain (unpreconditioned) QMR convenience overload.
template <typename T>
IterativeResult<crd::hesap::dense::RealType<T>> qmr(const crd::hesap::LinearOp<T>&                          a,
                                                    crd::containers::ConstSpan<T>                          b,
                                                    crd::containers::Span<T>                               x,
                                                    const IterativeOptions<crd::hesap::dense::RealType<T>>& opts,
                                                    QmrWorkspace<T>&                                       ws,
                                                    crd::memory::IAllocator* result_alloc)
{
    return qmr<T>(a, nullptr, b, x, opts, ws, result_alloc);
}

} // namespace crd::hesap::iterative
