#pragma once

// ode_adjoint.hpp — Phase 3.1.6 v16-f: ODE-ADJOINT unification + the discretize-then-optimize (DTO) vs
// continuous-adjoint (CTO) honesty split, over a self-contained explicit RK4 integrator. The RHS is a scalar-generic
// functor `void f(f64 t, const T* x, const T* θ, T* dx, int d, int np)` (T = f64 forward, `Var` for the taped adjoint)
// — one functor drives both. State d-dim, θ = np params, loss L(x_T) with dL/dx_T supplied.
//
//   • DTO (DEFAULT, EXACT): reverse-mode AD THROUGH the discrete integrator, ONE step at a time (`dto_step_vjp`), so
//     revolve.hpp checkpoints the forward states (O(snaps)=O(log T) memory) and the backward replays step-by-step.
//     The gradient is the EXACT gradient of the DISCRETE solve — consistent with the forward, ≡ central FD.
//   • CTO (O(state) memory, INCONSISTENT-CAVEAT): integrate the adjoint ODE backward with its OWN RK4 — λ̇ = −J_xᵀλ,
//     θ̄̇ = −(∂f/∂θ)ᵀλ, λ(T)=∂L/∂x_T (the CVODES-ASA / torchdiffeq-`odeint_adjoint` pattern). Because that RK4 is NOT
//     the transpose of the forward RK4, it is INCONSISTENT with the discrete forward ⇒ a gradient error growing with
//     the step (arXiv:2306.02192). Shipped with the caveat; DTO is the default.
//
// Self-contained (reuses tape.hpp); the production tape's {1..16} moat is untouched. Deterministic. ADR-0097.

#include <crd/hesap/autodiff/revolve.hpp>
#include <crd/hesap/autodiff/tape.hpp>

#include <crd/core/types.hpp>

namespace crd::hesap::autodiff::reverse
{

// One explicit RK4 step, generic over T (f64 forward / Var taped). `scratch` ≥ 5*d of T.
template <class T, class F>
inline void rk4_step(const F& f, const T* x, const T* theta, crd::f64 t, crd::f64 h, T* xn, int d, int np,
                     T* scratch) noexcept
{
    T* k1 = scratch;
    T* k2 = k1 + d;
    T* k3 = k2 + d;
    T* k4 = k3 + d;
    T* xs = k4 + d;
    f(t, x, theta, k1, d, np);
    for (int i = 0; i < d; ++i) { xs[i] = x[i] + (h * 0.5) * k1[i]; }
    f(t + h * 0.5, xs, theta, k2, d, np);
    for (int i = 0; i < d; ++i) { xs[i] = x[i] + (h * 0.5) * k2[i]; }
    f(t + h * 0.5, xs, theta, k3, d, np);
    for (int i = 0; i < d; ++i) { xs[i] = x[i] + h * k3[i]; }
    f(t + h, xs, theta, k4, d, np);
    for (int i = 0; i < d; ++i) { xn[i] = x[i] + (h / 6.0) * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]); }
}

// The DISCRETE adjoint of ONE RK4 step (via the tape): input state x, θ, output-state adjoint xbar_next → input-state
// adjoint xbar + ACCUMULATE theta_bar. `vscr` ≥ 7d+np Var (d x-leaves, np θ-leaves, d outputs, 5d step scratch).
template <class F>
inline void dto_step_vjp(const F& f, const crd::f64* x, const crd::f64* theta, crd::f64 t, crd::f64 h,
                         const crd::f64* xbar_next, crd::f64* xbar, crd::f64* theta_bar, int d, int np, Tape& tape,
                         Var* vscr) noexcept
{
    tape.reset();
    Var* xv  = vscr;
    Var* thv = xv + d;
    Var* xn  = thv + np;
    Var* sc  = xn + d;
    for (int i = 0; i < d; ++i) { xv[i] = make_leaf(tape, x[i]); }
    for (int j = 0; j < np; ++j) { thv[j] = make_leaf(tape, theta[j]); }
    rk4_step<Var>(f, xv, thv, t, h, xn, d, np, sc);
    for (int i = 0; i < d; ++i) { tape.seed(xn[i].node, xbar_next[i]); }
    tape.backward();
    for (int i = 0; i < d; ++i) { xbar[i] = tape.grad(xv[i].node); }
    for (int j = 0; j < np; ++j) { theta_bar[j] += tape.grad(thv[j].node); }
}

// ---- DTO gradient, STORE-ALL (the reference): O(T·d) memory, exact discrete gradient. ----
// x_all ≥ (T+1)*d, fscr ≥ 5d f64, vscr ≥ 7d+np Var, xbar/xbar_next ≥ d.
template <class F>
inline void dto_gradient(const F& f, const crd::f64* x0, const crd::f64* theta, int d, int np, int T, crd::f64 h,
                         const crd::f64* loss_grad, crd::f64* xbar0, crd::f64* theta_bar, crd::f64* x_all,
                         crd::f64* fscr, crd::f64* xbar, crd::f64* xbar_next, Tape& tape, Var* vscr) noexcept
{
    for (int i = 0; i < d; ++i) { x_all[i] = x0[i]; }
    for (int k = 0; k < T; ++k) { rk4_step<crd::f64>(f, x_all + k * d, theta, k * h, h, x_all + (k + 1) * d, d, np, fscr); }
    for (int j = 0; j < np; ++j) { theta_bar[j] = 0.0; }
    for (int i = 0; i < d; ++i) { xbar_next[i] = loss_grad[i]; }
    for (int k = T - 1; k >= 0; --k)
    {
        dto_step_vjp(f, x_all + k * d, theta, k * h, h, xbar_next, xbar, theta_bar, d, np, tape, vscr);
        for (int i = 0; i < d; ++i) { xbar_next[i] = xbar[i]; }
    }
    for (int i = 0; i < d; ++i) { xbar0[i] = xbar_next[i]; }
}

// ---- DTO gradient, REVOLVE-checkpointed: O(snaps·d) memory, GW-optimal recompute — bit-identical to store-all. ----
// ckpt ≥ snaps*d, work/xnext/xbar/xbar_next ≥ d, fscr ≥ 5d, vscr ≥ 7d+np Var, plan built for (T, snaps).
template <class F>
inline void dto_gradient_revolve(const F& f, const crd::f64* x0, const crd::f64* theta, int d, int np, int T,
                                 crd::f64 h, const crd::f64* loss_grad, crd::f64* xbar0, crd::f64* theta_bar,
                                 int snaps, const RevolvePlan& plan, crd::f64* ckpt, crd::f64* work, crd::f64* xnext,
                                 crd::f64* xbar, crd::f64* xbar_next, crd::f64* fscr, Tape& tape, Var* vscr) noexcept
{
    for (int j = 0; j < np; ++j) { theta_bar[j] = 0.0; }
    for (int i = 0; i < d; ++i) { work[i] = x0[i]; xbar_next[i] = loss_grad[i]; }
    revolve(
        plan, T, snaps,
        [&](int from, int to) noexcept // advance the working state from step `from` to `to`
        {
            for (int k = from; k < to; ++k)
            {
                rk4_step<crd::f64>(f, work, theta, k * h, h, xnext, d, np, fscr);
                for (int i = 0; i < d; ++i) { work[i] = xnext[i]; }
            }
        },
        [&](int slot) noexcept { for (int i = 0; i < d; ++i) { ckpt[slot * d + i] = work[i]; } },
        [&](int slot) noexcept { for (int i = 0; i < d; ++i) { work[i] = ckpt[slot * d + i]; } },
        [&](int step) noexcept // working state is at `step` (its input) → backprop
        {
            dto_step_vjp(f, work, theta, step * h, h, xbar_next, xbar, theta_bar, d, np, tape, vscr);
            for (int i = 0; i < d; ++i) { xbar_next[i] = xbar[i]; }
        });
    for (int i = 0; i < d; ++i) { xbar0[i] = xbar_next[i]; } // after reverse(0), xbar_next holds dL/dx_0
}

// ---- CTO: the continuous adjoint (inconsistent-caveat). ----
// RHS VJP: given x, θ, cotangent λ → gx = J_xᵀλ, gtheta += (∂f/∂θ)ᵀλ. `vscr` ≥ 2d+np Var.
template <class F>
inline void rhs_vjp(const F& f, const crd::f64* x, const crd::f64* theta, crd::f64 t, const crd::f64* lambda,
                    crd::f64* gx, crd::f64* gtheta, int d, int np, Tape& tape, Var* vscr) noexcept
{
    tape.reset();
    Var* xv  = vscr;
    Var* thv = xv + d;
    Var* dx  = thv + np;
    for (int i = 0; i < d; ++i) { xv[i] = make_leaf(tape, x[i]); }
    for (int j = 0; j < np; ++j) { thv[j] = make_leaf(tape, theta[j]); }
    f(t, xv, thv, dx, d, np);
    for (int i = 0; i < d; ++i) { tape.seed(dx[i].node, lambda[i]); }
    tape.backward();
    for (int i = 0; i < d; ++i) { gx[i] = tape.grad(xv[i].node); }
    for (int j = 0; j < np; ++j) { gtheta[j] += tape.grad(thv[j].node); }
}

// linear interpolation of the stored forward state at time t (grid t_k = k*h).
inline void interp_state(const crd::f64* x_all, int d, int T, crd::f64 h, crd::f64 t, crd::f64* out) noexcept
{
    crd::f64 s = t / h;
    if (s < 0.0) { s = 0.0; }
    if (s > static_cast<crd::f64>(T)) { s = static_cast<crd::f64>(T); }
    int            k = static_cast<int>(s);
    if (k >= T) { k = T - 1; }
    const crd::f64 w = s - static_cast<crd::f64>(k);
    for (int i = 0; i < d; ++i) { out[i] = (1.0 - w) * x_all[k * d + i] + w * x_all[(k + 1) * d + i]; }
}

// CTO gradient: forward-store x_all, then integrate [λ; θ̄] backward from T to 0 with RK4 (its own discretisation —
// NOT the transpose of the forward RK4 ⇒ the documented inconsistency). scratch: x_all (T+1)*d already filled by a
// prior dto_gradient/forward OR filled here; xi (d) interp state; kl (4*d) λ-stages; work λ buffers.
template <class F>
inline void cto_gradient(const F& f, const crd::f64* x0, const crd::f64* theta, int d, int np, int T, crd::f64 h,
                         const crd::f64* loss_grad, crd::f64* xbar0, crd::f64* theta_bar, crd::f64* x_all,
                         crd::f64* fscr, crd::f64* lam, crd::f64* xi, crd::f64* gth, Tape& tape, Var* vscr) noexcept
{
    for (int i = 0; i < d; ++i) { x_all[i] = x0[i]; }
    for (int k = 0; k < T; ++k) { rk4_step<crd::f64>(f, x_all + k * d, theta, k * h, h, x_all + (k + 1) * d, d, np, fscr); }
    for (int j = 0; j < np; ++j) { theta_bar[j] = 0.0; }
    for (int i = 0; i < d; ++i) { lam[i] = loss_grad[i]; }
    // Augmented backward RK4 for Y=[λ;θ̄]; RHS G(λ,t) = [−J_xᵀλ ; −(∂f/∂θ)ᵀλ], θ̄ integrated separately.
    // We need λ-stage evaluations: since θ̄ doesn't feed λ, integrate λ with RK4 and accumulate θ̄ with the same weights.
    crd::f64* s1 = fscr;       // λ-stage derivatives + a λ temp reuse fscr (≥5d)
    crd::f64* s2 = fscr + d;
    crd::f64* s3 = fscr + 2 * d;
    crd::f64* s4 = fscr + 3 * d;
    crd::f64* lt = fscr + 4 * d;
    // backward RK4 in reverse time (λ̇=−J_xᵀλ ⇒ backward: λ += (h/6)Σw_i·J_xᵀλ_i); θ̄ accumulated with the SAME weights.
    for (int k = T; k > 0; --k)
    {
        const crd::f64 t = k * h;
        interp_state(x_all, d, T, h, t, xi);
        for (int j = 0; j < np; ++j) { gth[j] = 0.0; }
        rhs_vjp(f, xi, theta, t, lam, s1, gth, d, np, tape, vscr);            // stage 1 (weight 1)
        for (int j = 0; j < np; ++j) { theta_bar[j] += (h / 6.0) * gth[j]; }
        for (int i = 0; i < d; ++i) { lt[i] = lam[i] + (h * 0.5) * s1[i]; }
        interp_state(x_all, d, T, h, t - h * 0.5, xi);
        for (int j = 0; j < np; ++j) { gth[j] = 0.0; }
        rhs_vjp(f, xi, theta, t - h * 0.5, lt, s2, gth, d, np, tape, vscr);   // stage 2 (weight 2)
        for (int j = 0; j < np; ++j) { theta_bar[j] += (h / 6.0) * 2.0 * gth[j]; }
        for (int i = 0; i < d; ++i) { lt[i] = lam[i] + (h * 0.5) * s2[i]; }
        for (int j = 0; j < np; ++j) { gth[j] = 0.0; }
        rhs_vjp(f, xi, theta, t - h * 0.5, lt, s3, gth, d, np, tape, vscr);   // stage 3 (weight 2)
        for (int j = 0; j < np; ++j) { theta_bar[j] += (h / 6.0) * 2.0 * gth[j]; }
        for (int i = 0; i < d; ++i) { lt[i] = lam[i] + h * s3[i]; }
        interp_state(x_all, d, T, h, t - h, xi);
        for (int j = 0; j < np; ++j) { gth[j] = 0.0; }
        rhs_vjp(f, xi, theta, t - h, lt, s4, gth, d, np, tape, vscr);         // stage 4 (weight 1)
        for (int j = 0; j < np; ++j) { theta_bar[j] += (h / 6.0) * gth[j]; }
        for (int i = 0; i < d; ++i) { lam[i] += (h / 6.0) * (s1[i] + 2.0 * s2[i] + 2.0 * s3[i] + s4[i]); }
    }
    for (int i = 0; i < d; ++i) { xbar0[i] = lam[i]; }
}

} // namespace crd::hesap::autodiff::reverse
