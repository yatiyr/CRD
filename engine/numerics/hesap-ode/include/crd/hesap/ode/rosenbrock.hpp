#pragma once

// rosenbrock.hpp — Phase 3.1.6 v9-f: Rosenbrock4 = RODAS4 (Hairer-Wanner IV.7) with Boost.odeint
// 1.83 EXACT semantics (`rosenbrock4.hpp` + `rosenbrock4_controller.hpp` read verbatim; coefficients
// extracted from the installed header — Hairer's rodas.f constants):
//   • 6 stages, ONE LU of (1/(γh)·I − J) and SIX back-solves per attempt, NO Newton iteration — the
//     bounded-cost real-time stiff stepper (the DAW / vehicle-drivetrain consumer; memory
//     `project_ode_in_games_layering`), stiffly accurate order 4 with the g6 stage as the embedded error,
//   • the odeint controller verbatim: err = RMS(xerr/(atol + rtol·max(|x|,|xold|))), fac =
//     clamp(err^0.25/0.9, 1/6, 5), dt_new = dt/fac, the Gustafsson predictive factor on accepts
//     (err_old floor 0.01), no-growth after a rejection,
//   • odeint recomputes J and the LU EVERY attempt (no reuse — its design, mirrored exactly so the
//     difftest counters compare 1:1).
// The linear solves run through the v9-d `OdeLinearSolver` seam: (1/(γh)·I − J)·g = r ⇔
// (I − γh·J)·g = γh·r (factor c = γh, scale the rhs). ∂f/∂t via forward differences (exactly zero for
// autonomous systems — one counted eval; an analytic-capability append is reserved). ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_linear_solver.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::ode
{

namespace detail
{

// RODAS4 (extracted from Boost.odeint 1.83 rosenbrock4.hpp — Hairer's rodas.f). ⚠ ONE NAMED FIX:
// odeint carries d4 = +0.0362…23 but Hairer's rodas.f has D4 = −0.0362…23 — the sign flip is invisible
// for autonomous systems (∂f/∂t = 0) and DEGRADES non-autonomous order from 4 to 1 (measured: odeint
// itself is asymptotically order 1 on y' = −2ty², p̂ = 1.0385 → 1.0002 over five h-decades; with the
// rodas.f sign our port restores order 4 — the convergence gate certifies it).
inline constexpr crd::f64 ros_gamma = 0.25;
inline constexpr crd::f64 ros_d[4] = {0.25, -0.1043, 0.1035, -0.3620000000000023e-01};
inline constexpr crd::f64 ros_c2 = 0.386;
inline constexpr crd::f64 ros_c3 = 0.21;
inline constexpr crd::f64 ros_c4 = 0.63;
inline constexpr crd::f64 ros_a21 = 0.1544000000000000e+01;
inline constexpr crd::f64 ros_a31 = 0.9466785280815826e+00;
inline constexpr crd::f64 ros_a32 = 0.2557011698983284e+00;
inline constexpr crd::f64 ros_a41 = 0.3314825187068521e+01;
inline constexpr crd::f64 ros_a42 = 0.2896124015972201e+01;
inline constexpr crd::f64 ros_a43 = 0.9986419139977817e+00;
inline constexpr crd::f64 ros_a51 = 0.1221224509226641e+01;
inline constexpr crd::f64 ros_a52 = 0.6019134481288629e+01;
inline constexpr crd::f64 ros_a53 = 0.1253708332932087e+02;
inline constexpr crd::f64 ros_a54 = -0.6878860361058950e+00;
inline constexpr crd::f64 ros_c21 = -0.5668800000000000e+01;
inline constexpr crd::f64 ros_c31 = -0.2430093356833875e+01;
inline constexpr crd::f64 ros_c32 = -0.2063599157091915e+00;
inline constexpr crd::f64 ros_c41 = -0.1073529058151375e+00;
inline constexpr crd::f64 ros_c42 = -0.9594562251023355e+01;
inline constexpr crd::f64 ros_c43 = -0.2047028614809616e+02;
inline constexpr crd::f64 ros_c51 = 0.7496443313967647e+01;
inline constexpr crd::f64 ros_c52 = -0.1024680431464352e+02;
inline constexpr crd::f64 ros_c53 = -0.3399990352819905e+02;
inline constexpr crd::f64 ros_c54 = 0.1170890893206160e+02;
inline constexpr crd::f64 ros_c61 = 0.8083246795921522e+01;
inline constexpr crd::f64 ros_c62 = -0.7981132988064893e+01;
inline constexpr crd::f64 ros_c63 = -0.3152159432874371e+02;
inline constexpr crd::f64 ros_c64 = 0.1631930543123136e+02;
inline constexpr crd::f64 ros_c65 = -0.6058818238834054e+01;

} // namespace detail

// RODAS4 adaptive driver (odeint rosenbrock4_controller semantics). `solver` nullptr ⇒ internal dense LU.
template <typename T>
[[nodiscard]] OdeResult<T> integrate_rosenbrock(const OdeFunction<T>& fn, T t0, T t1, crd::containers::Span<T> y,
                                                const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                                OdeLinearSolver<T>* solver = nullptr,
                                                OdeSolution<T>* solution = nullptr)
{
    namespace cont = crd::containers;
    const crd::usize n = fn.dim();
    CRD_ASSERT(y.size() == n);
    CRD_ASSERT(alloc != nullptr);

    OdeResult<T> result;
    result.t = t0;
    if (!std::isfinite(t0) || !std::isfinite(t1))
    {
        return result;
    }
    if (t1 == t0 || n == 0)
    {
        result.status = OdeStatus::Success;
        result.success = true;
        result.t = t1;
        if (solution != nullptr)
        {
            solution->reset(n);
        }
        return result;
    }

    DenseOdeLinearSolver<T> internal_solver(alloc);
    OdeLinearSolver<T>* lin = (solver != nullptr) ? solver : &internal_solver;

    const T direction = (t1 > t0) ? static_cast<T>(1) : static_cast<T>(-1);
    const T eps = std::numeric_limits<T>::epsilon();
    auto atol_i = [&opts](crd::usize i)
    {
        return opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
    };

    cont::Array<T> jac(alloc);
    jac.resize(n * n);
    cont::Array<T> f0(alloc);
    f0.resize(n);
    cont::Array<T> dfdt(alloc);
    dfdt.resize(n);
    cont::Array<T> g1(alloc);
    g1.resize(n);
    cont::Array<T> g2(alloc);
    g2.resize(n);
    cont::Array<T> g3(alloc);
    g3.resize(n);
    cont::Array<T> g4(alloc);
    g4.resize(n);
    cont::Array<T> g5(alloc);
    g5.resize(n);
    cont::Array<T> xtmp(alloc);
    xtmp.resize(n);
    cont::Array<T> ftmp(alloc);
    ftmp.resize(n);
    cont::Array<T> xerr(alloc);
    xerr.resize(n);
    cont::Array<T> xout(alloc);
    xout.resize(n);

    auto eval = [&fn, &result](T t, cont::ConstSpan<T> yy, cont::Span<T> out)
    {
        fn.rhs(t, yy, out);
        ++result.work.nfev;
    };
    auto solve_scaled = [&](T gh, cont::Span<T> rhs)
    {
        // (1/(γh)·I − J)·g = r ⇔ (I − γh·J)·g = γh·r
        for (crd::usize i = 0; i < n; ++i)
        {
            rhs[i] *= gh;
        }
        lin->solve(rhs);
        ++result.work.nsol;
    };

    // Initial step: user h0 or the Hairer heuristic (order-3 error estimator — the family convention).
    eval(t0, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(f0.data(), n));
    if (solution != nullptr)
    {
        solution->reset(n);
        solution->append(t0, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(f0.data(), n));
    }
    T h_abs;
    {
        const T interval_length = std::abs(t1 - t0);
        if (opts.h0 > static_cast<T>(0))
        {
            h_abs = std::abs(opts.h0) < interval_length ? std::abs(opts.h0) : interval_length;
        }
        else
        {
            T d0s = static_cast<T>(0);
            T d1s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
                d0s += (y[i] / sc) * (y[i] / sc);
                d1s += (f0[i] / sc) * (f0[i] / sc);
            }
            const T d0 = crd::math::sqrt(d0s / static_cast<T>(n));
            const T d1 = crd::math::sqrt(d1s / static_cast<T>(n));
            T h0_try = (d0 < static_cast<T>(1e-5) || d1 < static_cast<T>(1e-5)) ? static_cast<T>(1e-6)
                                                                                : static_cast<T>(0.01) * d0 / d1;
            h0_try = h0_try < interval_length ? h0_try : interval_length;
            for (crd::usize i = 0; i < n; ++i)
            {
                xtmp[i] = y[i] + h0_try * direction * f0[i];
            }
            eval(t0 + h0_try * direction, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
            T d2s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
                const T q = (ftmp[i] - f0[i]) / sc;
                d2s += q * q;
            }
            const T d2 = crd::math::sqrt(d2s / static_cast<T>(n)) / h0_try;
            T h1;
            if (d1 <= static_cast<T>(1e-15) && d2 <= static_cast<T>(1e-15))
            {
                h1 = static_cast<T>(1e-6) > h0_try * static_cast<T>(1e-3) ? static_cast<T>(1e-6)
                                                                          : h0_try * static_cast<T>(1e-3);
            }
            else
            {
                const T dm = d1 > d2 ? d1 : d2;
                h1 = crd::math::pow(static_cast<T>(0.01) / dm, static_cast<T>(1) / static_cast<T>(4));
            }
            h_abs = static_cast<T>(100) * h0_try;
            h_abs = h_abs < h1 ? h_abs : h1;
            h_abs = h_abs < interval_length ? h_abs : interval_length;
            h_abs = h_abs < opts.hmax ? h_abs : opts.hmax;
        }
    }

    // odeint controller state.
    bool first_step = true;
    bool last_rejected = false;
    T dt_old = static_cast<T>(0);
    T err_old = static_cast<T>(0);

    T t = t0;
    const T inf_t = std::numeric_limits<T>::infinity();

    while (t != t1)
    {
        if (opts.max_steps != 0 && result.work.nsteps >= opts.max_steps)
        {
            result.status = OdeStatus::MaxSteps;
            result.t = t;
            return result;
        }
        const T min_step = static_cast<T>(10) * std::abs(std::nextafter(t, direction * inf_t) - t);
        if (h_abs < min_step)
        {
            result.status = OdeStatus::StepTooSmall;
            result.t = t;
            return result;
        }
        if (h_abs > opts.hmax)
        {
            h_abs = opts.hmax;
        }
        T h = h_abs * direction;
        T t_new = t + h;
        if (direction * (t_new - t1) > static_cast<T>(0))
        {
            t_new = t1;
        }
        h = t_new - t;
        h_abs = std::abs(h);

        // --- the RODAS4 attempt (odeint do_step verbatim; J + LU rebuilt every attempt, as odeint does) ---
        eval(t, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(f0.data(), n));
        if (fn.has_jacobian())
        {
            const bool ok = fn.jacobian(t, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(jac.data(), n * n));
            CRD_ASSERT(ok);
            (void)ok;
            ++result.work.njev;
        }
        else
        {
            const T sqrt_eps = crd::math::sqrt(eps);
            for (crd::usize i = 0; i < n; ++i)
            {
                xtmp[i] = y[i];
            }
            for (crd::usize j = 0; j < n; ++j)
            {
                const T yj = xtmp[j];
                const T mag = std::abs(yj) > static_cast<T>(1) ? std::abs(yj) : static_cast<T>(1);
                const T hj = sqrt_eps * mag;
                xtmp[j] = yj + hj;
                eval(t, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
                for (crd::usize i = 0; i < n; ++i)
                {
                    jac[i * n + j] = (ftmp[i] - f0[i]) / hj;
                }
                xtmp[j] = yj;
            }
            ++result.work.njev;
        }
        // ∂f/∂t by forward difference (exactly 0 for autonomous f).
        {
            const T dt_t = crd::math::sqrt(eps) * (std::abs(t) > static_cast<T>(1) ? std::abs(t) : static_cast<T>(1));
            eval(t + dt_t, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(dfdt.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                dfdt[i] = (dfdt[i] - f0[i]) / dt_t;
            }
        }

        const T gh = static_cast<T>(detail::ros_gamma) * h;
        ++result.work.nlu;
        if (!lin->factor_iteration_matrix(gh, cont::ConstSpan<T>(jac.data(), n * n), n))
        {
            result.status = OdeStatus::StepTooSmall; // singular iteration matrix — honest stop
            result.t = t;
            return result;
        }

        for (crd::usize i = 0; i < n; ++i)
        {
            g1[i] = f0[i] + h * static_cast<T>(detail::ros_d[0]) * dfdt[i];
        }
        solve_scaled(gh, cont::Span<T>(g1.data(), n));

        for (crd::usize i = 0; i < n; ++i)
        {
            xtmp[i] = y[i] + static_cast<T>(detail::ros_a21) * g1[i];
        }
        eval(t + static_cast<T>(detail::ros_c2) * h, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            g2[i] =
                ftmp[i] + h * static_cast<T>(detail::ros_d[1]) * dfdt[i] + static_cast<T>(detail::ros_c21) * g1[i] / h;
        }
        solve_scaled(gh, cont::Span<T>(g2.data(), n));

        for (crd::usize i = 0; i < n; ++i)
        {
            xtmp[i] = y[i] + static_cast<T>(detail::ros_a31) * g1[i] + static_cast<T>(detail::ros_a32) * g2[i];
        }
        eval(t + static_cast<T>(detail::ros_c3) * h, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            g3[i] = ftmp[i] + h * static_cast<T>(detail::ros_d[2]) * dfdt[i] +
                    (static_cast<T>(detail::ros_c31) * g1[i] + static_cast<T>(detail::ros_c32) * g2[i]) / h;
        }
        solve_scaled(gh, cont::Span<T>(g3.data(), n));

        for (crd::usize i = 0; i < n; ++i)
        {
            xtmp[i] = y[i] + static_cast<T>(detail::ros_a41) * g1[i] + static_cast<T>(detail::ros_a42) * g2[i] +
                      static_cast<T>(detail::ros_a43) * g3[i];
        }
        eval(t + static_cast<T>(detail::ros_c4) * h, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            g4[i] = ftmp[i] + h * static_cast<T>(detail::ros_d[3]) * dfdt[i] +
                    (static_cast<T>(detail::ros_c41) * g1[i] + static_cast<T>(detail::ros_c42) * g2[i] +
                     static_cast<T>(detail::ros_c43) * g3[i]) /
                        h;
        }
        solve_scaled(gh, cont::Span<T>(g4.data(), n));

        for (crd::usize i = 0; i < n; ++i)
        {
            xtmp[i] = y[i] + static_cast<T>(detail::ros_a51) * g1[i] + static_cast<T>(detail::ros_a52) * g2[i] +
                      static_cast<T>(detail::ros_a53) * g3[i] + static_cast<T>(detail::ros_a54) * g4[i];
        }
        eval(t + h, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            g5[i] = ftmp[i] + (static_cast<T>(detail::ros_c51) * g1[i] + static_cast<T>(detail::ros_c52) * g2[i] +
                               static_cast<T>(detail::ros_c53) * g3[i] + static_cast<T>(detail::ros_c54) * g4[i]) /
                                  h;
        }
        solve_scaled(gh, cont::Span<T>(g5.data(), n));

        for (crd::usize i = 0; i < n; ++i)
        {
            xtmp[i] += g5[i];
        }
        eval(t + h, cont::ConstSpan<T>(xtmp.data(), n), cont::Span<T>(ftmp.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            xerr[i] = ftmp[i] + (static_cast<T>(detail::ros_c61) * g1[i] + static_cast<T>(detail::ros_c62) * g2[i] +
                                 static_cast<T>(detail::ros_c63) * g3[i] + static_cast<T>(detail::ros_c64) * g4[i] +
                                 static_cast<T>(detail::ros_c65) * g5[i]) /
                                    h;
        }
        solve_scaled(gh, cont::Span<T>(xerr.data(), n));
        for (crd::usize i = 0; i < n; ++i)
        {
            xout[i] = xtmp[i] + xerr[i];
        }

        // --- odeint controller (verbatim) ---
        T esum = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T sk =
                atol_i(i) + opts.rtol * (std::abs(y[i]) > std::abs(xout[i]) ? std::abs(y[i]) : std::abs(xout[i]));
            esum += (xerr[i] / sk) * (xerr[i] / sk);
        }
        const T err = crd::math::sqrt(esum / static_cast<T>(n));
        ++result.work.nsteps;

        const T safe = static_cast<T>(0.9);
        const T fac1 = static_cast<T>(5);
        const T fac2 = static_cast<T>(1) / static_cast<T>(6);
        T fac = crd::math::pow(err, static_cast<T>(0.25)) / safe;
        fac = fac < fac1 ? fac : fac1;
        fac = fac > fac2 ? fac : fac2;
        T dt_new_abs = h_abs / fac;

        if (err <= static_cast<T>(1))
        {
            if (first_step)
            {
                first_step = false;
            }
            else
            {
                T fac_pred = (dt_old / h_abs) * crd::math::pow(err * err / err_old, static_cast<T>(0.25)) / safe;
                fac_pred = fac_pred < fac1 ? fac_pred : fac1;
                fac_pred = fac_pred > fac2 ? fac_pred : fac2;
                fac = fac > fac_pred ? fac : fac_pred;
                dt_new_abs = h_abs / fac;
            }
            dt_old = h_abs;
            err_old = err > static_cast<T>(0.01) ? err : static_cast<T>(0.01);
            if (last_rejected)
            {
                dt_new_abs = dt_new_abs < h_abs ? dt_new_abs : h_abs;
            }
            last_rejected = false;
            ++result.work.naccept;

            t = t_new;
            bool finite = true;
            for (crd::usize i = 0; i < n; ++i)
            {
                y[i] = xout[i];
                finite = finite && std::isfinite(y[i]);
            }
            if (!finite)
            {
                result.status = OdeStatus::NotFinite;
                result.t = t;
                return result;
            }
            if (solution != nullptr)
            {
                // One extra (counted) eval: the Hermite node needs f(t_new, y_new), which RODAS4's
                // stage structure does not leave behind (disabled in the counter difftest).
                eval(t, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(ftmp.data(), n));
                solution->append(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(ftmp.data(), n));
            }
            h_abs = dt_new_abs;
        }
        else
        {
            last_rejected = true;
            ++result.work.nreject;
            h_abs = dt_new_abs;
        }
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
