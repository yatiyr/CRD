#pragma once

// steppers.hpp — Phase 3.1.6 v9-a: the KERNEL layer of crd-hesap-ode (ADR-0091). Raw-span, allocation-free,
// inlineable fixed-step explicit steppers — designed so hot loops (eylem's fused SoA body sweep, animation
// spring chains, DAW per-sample integration) consume them WITHOUT the driver: no virtual dispatch, no
// allocation, caller-owned scratch (memory `project_ode_in_games_layering`). The general `integrate_fixed`
// driver (integrate.hpp) is a thin loop over these same kernels — one implementation, two API layers.
//
// CONTRACT (all kernels):
//   • F is any callable `f(T t, ConstSpan<const T> y, Span<T> dydt)` evaluating the RHS y' = f(t, y).
//   • `scratch.size() >= *_scratch(n)` (asserted). Kernels never allocate.
//   • IN-PLACE SAFE: `y_out` may alias `y` (every kernel finishes reading y[i] before writing y_out[i]).
//   • DETERMINISM: fixed operation order per element; element-independent loops (auto-vectorization cannot
//     change any element's FP chain) — bit-identical across runs, build configs aside.
//   • Raw lower-layer per ADR-0078 §5 (like the gemm microkernels): no Quantity tags below the API surface.
//
// The symplectic family (semi-implicit Euler / Verlet / Yoshida — the eylem game-mode steppers) lands as
// v9-g in this same header style; embedded adaptive pairs (RK45/DOP853/Tsit5...) land as v9-b.

#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>

namespace crd::hesap::ode
{

// Scratch requirements (in elements of T) for each kernel at state dimension n.
[[nodiscard]] constexpr crd::usize euler_scratch(crd::usize n) noexcept
{
    return n; // k1
}
[[nodiscard]] constexpr crd::usize midpoint_scratch(crd::usize n) noexcept
{
    return 2 * n; // k, ytmp
}
[[nodiscard]] constexpr crd::usize rk4_scratch(crd::usize n) noexcept
{
    return 3 * n; // k, ytmp, acc
}

// Forward Euler: y_out = y + h·f(t, y). Order 1. 1 RHS eval.
template <typename T, typename F>
void step_euler(F&& f, T t, crd::containers::ConstSpan<T> y, T h, crd::containers::Span<T> y_out,
                crd::containers::Span<T> scratch)
{
    const crd::usize n = y.size();
    CRD_ASSERT(y_out.size() == n);
    CRD_ASSERT(scratch.size() >= euler_scratch(n));
    crd::containers::Span<T> k = scratch.subspan(0, n);

    f(t, y, k);
    for (crd::usize i = 0; i < n; ++i)
    {
        y_out[i] = y[i] + h * k[i];
    }
}

// Explicit midpoint (RK2): k1 at t, k2 at the midpoint, y_out = y + h·k2. Order 2. 2 RHS evals.
template <typename T, typename F>
void step_midpoint(F&& f, T t, crd::containers::ConstSpan<T> y, T h, crd::containers::Span<T> y_out,
                   crd::containers::Span<T> scratch)
{
    const crd::usize n = y.size();
    CRD_ASSERT(y_out.size() == n);
    CRD_ASSERT(scratch.size() >= midpoint_scratch(n));
    crd::containers::Span<T> k = scratch.subspan(0, n);
    crd::containers::Span<T> ytmp = scratch.subspan(n, n);

    const T half_h = h * static_cast<T>(0.5);

    f(t, y, k);
    for (crd::usize i = 0; i < n; ++i)
    {
        ytmp[i] = y[i] + half_h * k[i];
    }
    f(t + half_h, crd::containers::ConstSpan<T>(ytmp), k);
    for (crd::usize i = 0; i < n; ++i)
    {
        y_out[i] = y[i] + h * k[i];
    }
}

// Classical RK4: y_out = y + (h/6)·(k1 + 2k2 + 2k3 + k4). Order 4. 4 RHS evals.
template <typename T, typename F>
void step_rk4(F&& f, T t, crd::containers::ConstSpan<T> y, T h, crd::containers::Span<T> y_out,
              crd::containers::Span<T> scratch)
{
    const crd::usize n = y.size();
    CRD_ASSERT(y_out.size() == n);
    CRD_ASSERT(scratch.size() >= rk4_scratch(n));
    crd::containers::Span<T> k = scratch.subspan(0, n);
    crd::containers::Span<T> ytmp = scratch.subspan(n, n);
    crd::containers::Span<T> acc = scratch.subspan(2 * n, n);

    const T half_h = h * static_cast<T>(0.5);
    const T two = static_cast<T>(2);

    // k1
    f(t, y, k);
    for (crd::usize i = 0; i < n; ++i)
    {
        acc[i] = k[i];
        ytmp[i] = y[i] + half_h * k[i];
    }
    // k2
    f(t + half_h, crd::containers::ConstSpan<T>(ytmp), k);
    for (crd::usize i = 0; i < n; ++i)
    {
        acc[i] += two * k[i];
        ytmp[i] = y[i] + half_h * k[i];
    }
    // k3
    f(t + half_h, crd::containers::ConstSpan<T>(ytmp), k);
    for (crd::usize i = 0; i < n; ++i)
    {
        acc[i] += two * k[i];
        ytmp[i] = y[i] + h * k[i];
    }
    // k4
    f(t + h, crd::containers::ConstSpan<T>(ytmp), k);
    const T h_sixth = h / static_cast<T>(6);
    for (crd::usize i = 0; i < n; ++i)
    {
        acc[i] += k[i];
        y_out[i] = y[i] + h_sixth * acc[i];
    }
}

} // namespace crd::hesap::ode
