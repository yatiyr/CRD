#pragma once

// sdirk.hpp — Phase 3.1.6 v9-f: TR-BDF2 as a 3-stage stiffly-accurate ESDIRK (Hosea-Shampine 1996 — the
// MATLAB ode23tb / SPICE-class circuit workhorse, THE SDIRK shape the v9-i IMEX implicit cores reuse).
// Butcher table (γ = 2 − √2, exact closed forms; the ARKODE-registered TRBDF2 table):
//   c = [0, γ, 1];  A = [[0,0,0], [γ/2, γ/2, 0], [√2/4, √2/4, γ/2]];  b = A[2] (stiffly accurate)
//   b̂ = [(1 − √2/4)/3, (3√2/4 + 1)/3, γ/6]  (the embedded 2nd-order error weights; Σb̂ = 1)
// Both implicit stages share d = γ/2 ⇒ ONE iteration matrix (I − d·h·J) per h — the SDIRK selling point.
// Simplified Newton per implicit stage (the BDF v9-d machinery: rate predicates, Jacobian refresh then
// h-halving on convergence failure); L-stable; first stage FSAL-free explicit (k1 = f(t, y)).
// Controller: the v9-a scipy ElementaryController with exponent −1/3 (embedded estimator order 2).
// Linear algebra through the v9-d `OdeLinearSolver` seam. ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/controller.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_linear_solver.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::ode
{

namespace detail
{

inline constexpr crd::f64 trbdf2_sqrt2 = 1.4142135623730951;
inline constexpr crd::f64 trbdf2_gamma = 2.0 - trbdf2_sqrt2;
inline constexpr crd::f64 trbdf2_d = trbdf2_gamma / 2.0; // the shared diagonal
inline constexpr crd::f64 trbdf2_b[3] = {trbdf2_sqrt2 / 4.0, trbdf2_sqrt2 / 4.0, trbdf2_gamma / 2.0};
inline constexpr crd::f64 trbdf2_bhat[3] = {(1.0 - trbdf2_sqrt2 / 4.0) / 3.0, (3.0 * trbdf2_sqrt2 / 4.0 + 1.0) / 3.0,
                                            trbdf2_gamma / 6.0};
inline constexpr crd::u32 trbdf2_newton_maxiter = 4;

} // namespace detail

// TR-BDF2 adaptive driver. `solver` nullptr ⇒ internal dense LU.
template <typename T>
[[nodiscard]] OdeResult<T> integrate_trbdf2(const OdeFunction<T>& fn, T t0, T t1, crd::containers::Span<T> y,
                                            const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                            OdeLinearSolver<T>* solver = nullptr, OdeSolution<T>* solution = nullptr)
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
    const T nt_lo = static_cast<T>(10) * eps / opts.rtol;
    const T nt_hi = std::sqrt(opts.rtol) < static_cast<T>(0.03) ? std::sqrt(opts.rtol) : static_cast<T>(0.03);
    const T newton_tol = nt_lo > nt_hi ? nt_lo : nt_hi;
    auto atol_i = [&opts](crd::usize i)
    {
        return opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
    };

    cont::Array<T> jac(alloc);
    jac.resize(n * n);
    cont::Array<T> k1(alloc);
    k1.resize(n);
    cont::Array<T> k2(alloc);
    k2.resize(n);
    cont::Array<T> k3(alloc);
    k3.resize(n);
    cont::Array<T> ys(alloc);
    ys.resize(n);
    cont::Array<T> rhs(alloc);
    rhs.resize(n);
    cont::Array<T> ytmp(alloc);
    ytmp.resize(n);
    cont::Array<T> scale(alloc);
    scale.resize(n);

    auto eval = [&fn, &result](T t, cont::ConstSpan<T> yy, cont::Span<T> out)
    {
        fn.rhs(t, yy, out);
        ++result.work.nfev;
    };
    auto build_jacobian = [&](T t, cont::ConstSpan<T> yy, cont::ConstSpan<T> f_at)
    {
        if (fn.has_jacobian())
        {
            const bool ok = fn.jacobian(t, yy, cont::Span<T>(jac.data(), n * n));
            CRD_ASSERT(ok);
            (void)ok;
            ++result.work.njev;
            return;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            ytmp[i] = yy[i];
        }
        const T sqrt_eps = std::sqrt(eps);
        for (crd::usize j = 0; j < n; ++j)
        {
            const T yj = ytmp[j];
            const T mag = std::abs(yj) > static_cast<T>(1) ? std::abs(yj) : static_cast<T>(1);
            const T hj = sqrt_eps * mag;
            ytmp[j] = yj + hj;
            eval(t, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(rhs.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                jac[i * n + j] = (rhs[i] - f_at[i]) / hj;
            }
            ytmp[j] = yj;
        }
        ++result.work.njev;
    };

    // Simplified Newton for an implicit stage: y_s = base + d·h·f(t_s, y_s); ys holds the start iterate.
    // Returns true on convergence; counts evals/solves.
    auto stage_newton = [&](T t_s, T dh, cont::ConstSpan<T> base, cont::Span<T> f_out)
    {
        T dy_norm_old = static_cast<T>(-1);
        for (crd::u32 k = 0; k < detail::trbdf2_newton_maxiter; ++k)
        {
            eval(t_s, cont::ConstSpan<T>(ys.data(), n), f_out);
            bool finite = true;
            for (crd::usize i = 0; i < n; ++i)
            {
                finite = finite && std::isfinite(f_out[i]);
            }
            if (!finite)
            {
                return false;
            }
            // residual r = base + d·h·f − ys; solve (I − d·h·J)·dy = r
            for (crd::usize i = 0; i < n; ++i)
            {
                rhs[i] = base[i] + dh * f_out[i] - ys[i];
            }
            lin->solve(cont::Span<T>(rhs.data(), n));
            ++result.work.nsol;
            T sum = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T q = rhs[i] / scale[i];
                sum += q * q;
            }
            const T dy_norm = std::sqrt(sum / static_cast<T>(n));
            const bool have_rate = dy_norm_old >= static_cast<T>(0);
            const T rate = have_rate ? dy_norm / dy_norm_old : static_cast<T>(0);
            if (have_rate && rate >= static_cast<T>(1))
            {
                return false;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                ys[i] += rhs[i];
            }
            if (dy_norm == static_cast<T>(0) || (have_rate && rate / (static_cast<T>(1) - rate) * dy_norm < newton_tol))
            {
                return true;
            }
            dy_norm_old = dy_norm;
        }
        return false;
    };

    // f0 + initial h (Hairer heuristic, estimator order 2 ⇒ exponent 1/3) + initial Jacobian.
    eval(t0, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(k1.data(), n));
    if (solution != nullptr)
    {
        solution->reset(n);
        solution->append(t0, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(k1.data(), n));
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
                d1s += (k1[i] / sc) * (k1[i] / sc);
            }
            const T d0 = std::sqrt(d0s / static_cast<T>(n));
            const T d1 = std::sqrt(d1s / static_cast<T>(n));
            T h0_try = (d0 < static_cast<T>(1e-5) || d1 < static_cast<T>(1e-5)) ? static_cast<T>(1e-6)
                                                                                : static_cast<T>(0.01) * d0 / d1;
            h0_try = h0_try < interval_length ? h0_try : interval_length;
            for (crd::usize i = 0; i < n; ++i)
            {
                ytmp[i] = y[i] + h0_try * direction * k1[i];
            }
            eval(t0 + h0_try * direction, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(rhs.data(), n));
            T d2s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
                const T q = (rhs[i] - k1[i]) / sc;
                d2s += q * q;
            }
            const T d2 = std::sqrt(d2s / static_cast<T>(n)) / h0_try;
            T h1;
            if (d1 <= static_cast<T>(1e-15) && d2 <= static_cast<T>(1e-15))
            {
                h1 = static_cast<T>(1e-6) > h0_try * static_cast<T>(1e-3) ? static_cast<T>(1e-6)
                                                                          : h0_try * static_cast<T>(1e-3);
            }
            else
            {
                const T dm = d1 > d2 ? d1 : d2;
                h1 = std::pow(static_cast<T>(0.01) / dm, static_cast<T>(1) / static_cast<T>(3));
            }
            h_abs = static_cast<T>(100) * h0_try;
            h_abs = h_abs < h1 ? h_abs : h1;
            h_abs = h_abs < interval_length ? h_abs : interval_length;
            h_abs = h_abs < opts.hmax ? h_abs : opts.hmax;
        }
    }
    build_jacobian(t0, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(k1.data(), n));
    bool current_jac = true;
    bool lu_valid = false;
    T lu_h = static_cast<T>(0);

    ElementaryController<T> controller;
    controller.exponent = static_cast<T>(-1) / static_cast<T>(3); // estimator order 2

    T t = t0;
    const T inf_t = std::numeric_limits<T>::infinity();
    const T g = static_cast<T>(detail::trbdf2_gamma);
    const T d = static_cast<T>(detail::trbdf2_d);

    while (t != t1)
    {
        if (opts.max_steps != 0 && result.work.nsteps >= opts.max_steps)
        {
            result.status = OdeStatus::MaxSteps;
            result.t = t;
            return result;
        }
        const T min_step = static_cast<T>(10) * std::abs(std::nextafter(t, direction * inf_t) - t);
        if (h_abs > opts.hmax)
        {
            h_abs = opts.hmax;
        }

        bool step_accepted = false;
        T t_new = t;
        current_jac = false; // refreshable per outer step (the BDF convention)

        while (!step_accepted)
        {
            if (h_abs < min_step)
            {
                result.status = OdeStatus::StepTooSmall;
                result.t = t;
                return result;
            }
            T h = h_abs * direction;
            t_new = t + h;
            if (direction * (t_new - t1) > static_cast<T>(0))
            {
                t_new = t1;
            }
            h = t_new - t;
            h_abs = std::abs(h);
            const T dh = d * h;

            // k1 = f(t, y) is current in k1 (refreshed on accept).
            for (crd::usize i = 0; i < n; ++i)
            {
                scale[i] = atol_i(i) + std::abs(y[i]) * opts.rtol;
            }

            bool converged = false;
            while (!converged)
            {
                if (!lu_valid || lu_h != dh)
                {
                    const bool ok = lin->factor_iteration_matrix(dh, cont::ConstSpan<T>(jac.data(), n * n), n);
                    ++result.work.nlu;
                    lu_valid = ok;
                    lu_h = dh;
                    if (!ok)
                    {
                        break;
                    }
                }
                // Stage 2 (TR): y_g = y + (γh/2)(k1 + k2); base = y + d·h·k1; start iterate = y.
                for (crd::usize i = 0; i < n; ++i)
                {
                    ytmp[i] = y[i] + dh * k1[i]; // base
                    ys[i] = y[i];                // predictor
                }
                converged =
                    stage_newton(t + g * h, dh, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(k2.data(), n));
                if (converged)
                {
                    // Stage 3 (BDF2): y_new = y + h(b1·k1 + b2·k2) + d·h·k3; start iterate = y_g.
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        ytmp[i] = y[i] + h * (static_cast<T>(detail::trbdf2_b[0]) * k1[i] +
                                              static_cast<T>(detail::trbdf2_b[1]) * k2[i]);
                        // ys keeps the stage-2 solution as the predictor
                    }
                    converged =
                        stage_newton(t_new, dh, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(k3.data(), n));
                }
                if (!converged)
                {
                    if (current_jac)
                    {
                        break;
                    }
                    build_jacobian(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(k1.data(), n));
                    current_jac = true;
                    lu_valid = false;
                }
            }

            ++result.work.nsteps;
            if (!converged)
            {
                h_abs *= static_cast<T>(0.5);
                lu_valid = false;
                ++result.work.nreject;
                continue;
            }

            // y_new is the converged stage-3 iterate (stiffly accurate); error = h·Σ(b−b̂)·k.
            T esum = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T e_i =
                    h * ((static_cast<T>(detail::trbdf2_b[0]) - static_cast<T>(detail::trbdf2_bhat[0])) * k1[i] +
                         (static_cast<T>(detail::trbdf2_b[1]) - static_cast<T>(detail::trbdf2_bhat[1])) * k2[i] +
                         (static_cast<T>(detail::trbdf2_b[2]) - static_cast<T>(detail::trbdf2_bhat[2])) * k3[i]);
                const T sk =
                    atol_i(i) + opts.rtol * (std::abs(y[i]) > std::abs(ys[i]) ? std::abs(y[i]) : std::abs(ys[i]));
                esum += (e_i / sk) * (e_i / sk);
            }
            const T error_norm = std::sqrt(esum / static_cast<T>(n));

            bool accept = false;
            const T factor = controller.update(error_norm, accept);
            if (accept)
            {
                step_accepted = true;
                ++result.work.naccept;
                t = t_new;
                bool finite = true;
                for (crd::usize i = 0; i < n; ++i)
                {
                    y[i] = ys[i];
                    finite = finite && std::isfinite(y[i]);
                }
                if (!finite)
                {
                    result.status = OdeStatus::NotFinite;
                    result.t = t;
                    return result;
                }
                for (crd::usize i = 0; i < n; ++i)
                {
                    k1[i] = k3[i]; // FSAL: k3 = f(t_new, y_new) exactly (stiffly accurate)
                }
                if (solution != nullptr)
                {
                    solution->append(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(k1.data(), n));
                }
            }
            else
            {
                ++result.work.nreject;
            }
            const T h_next = h_abs * factor;
            if (h_next != h_abs)
            {
                lu_valid = false; // dh changes ⇒ refactor (checked via lu_h anyway; explicit for clarity)
            }
            h_abs = h_next;
        }
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
