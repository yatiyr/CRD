#pragma once

// imex.hpp — Phase 3.1.6 v9-i: IMEX additive Runge-Kutta (the method-of-lines / CFD pull). The RHS splits
// f = f_E + f_I: f_E (the EXPLICIT, non-stiff part — advection, reaction) is advanced by an explicit RK
// tableau; f_I (the IMPLICIT, stiff part — diffusion) by a stiffly-accurate ESDIRK tableau via simplified
// Newton through the v9-d OdeLinearSolver seam. The two tableaus share b/d/c (the ARK[2]SA property) so the
// combined embedded estimator is one (b − d) weighting of the combined stage derivatives.
//
// METHODS (Kennedy & Carpenter 2003 — ARKODE's ARKStep IMEX defaults, so v9-z benches apples-to-apples):
//   ARK3(2)4L[2]SA (q=3,p=2) · ARK4(3)6L[2]SA (q=4,p=3) · ARK5(4)8L[2]SA (q=5,p=4). Tableaus EXTRACTED from
//   the installed SUNDIALS v6.4.1 by scripts/gen_ark_tableaus.py into detail/ark_tableaus.hpp (bit-identical
//   to ARKODE — extraction beats transcription; the d4-sign lesson). Per-part order-slope gates certify each.
//
// ECONOMY: all implicit stages share the diagonal γ ⇒ ONE iteration matrix (I − γh·J_I) per step (the SDIRK
// selling point). FSAL: stage 0 is at the accepted point, so f_E/f_I there carry from the previous step's
// y_new — one stage-0-worth of evals per accepted step (also the dense-output node). Simplified Newton with
// the BDF/SDIRK machinery (rate predicate, Jacobian refresh then h-halving on failure). Controller: the
// v9-a scipy ElementaryController, exponent −1/(p+1). MOAT: pure deterministic FP — bit-identical run-twice.
// ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/controller.hpp>
#include <crd/hesap/ode/detail/ark_tableaus.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_linear_solver.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::ode
{

enum class ImexMethod : crd::u8
{
    Ark3 = 0, // ARK3(2)4L[2]SA: order 3, embedded estimator 2
    Ark4 = 1, // ARK4(3)6L[2]SA: order 4, embedded estimator 3
    Ark5 = 2, // ARK5(4)8L[2]SA: order 5, embedded estimator 4
};

namespace detail
{

inline constexpr crd::u32 imex_newton_maxiter = 4;

struct ImexDesc
{
    const crd::f64* ae; // explicit A, row-major stages×stages (strictly lower)
    const crd::f64* ai; // implicit A, row-major stages×stages (lower incl. diagonal γ)
    const crd::f64* b;  // solution weights (shared)
    const crd::f64* d;  // embedded weights (shared)
    const crd::f64* c;  // abscissae (shared)
    crd::usize stages;
    crd::i32 error_estimator_order; // p
};

[[nodiscard]] inline ImexDesc imex_desc(ImexMethod m) noexcept
{
    switch (m)
    {
        case ImexMethod::Ark3:
            return {ark3_ae, ark3_ai, ark3_b, ark3_d, ark3_c, 4, 2};
        case ImexMethod::Ark4:
            return {ark4_ae, ark4_ai, ark4_b, ark4_d, ark4_c, 6, 3};
        case ImexMethod::Ark5:
        default:
            return {ark5_ae, ark5_ai, ark5_b, ark5_d, ark5_c, 8, 4};
    }
}

} // namespace detail

// IMEX additive-RK adaptive driver. `fn` MUST be an IMEX function (has_imex_split()); `rhs` = f_E + f_I.
// `solver` nullptr ⇒ internal dense LU. nfev counts HALF-evaluations (each f_E or f_I call = +1).
template <typename T>
[[nodiscard]] OdeResult<T> integrate_imex(const OdeFunction<T>& fn, T t0, T t1, crd::containers::Span<T> y,
                                          const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                          ImexMethod method = ImexMethod::Ark4, OdeLinearSolver<T>* solver = nullptr,
                                          OdeSolution<T>* solution = nullptr)
{
    namespace cont = crd::containers;
    const crd::usize n = fn.dim();
    CRD_ASSERT(y.size() == n);
    CRD_ASSERT(alloc != nullptr);
    CRD_ASSERT(fn.has_imex_split());

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

    const detail::ImexDesc desc = detail::imex_desc(method);
    const crd::usize s = desc.stages;
    const T gamma = static_cast<T>(desc.ai[1 * s + 1]); // shared ESDIRK diagonal

    DenseOdeLinearSolver<T> internal_solver(alloc);
    OdeLinearSolver<T>* lin = (solver != nullptr) ? solver : &internal_solver;

    const T direction = (t1 > t0) ? static_cast<T>(1) : static_cast<T>(-1);
    const T eps = std::numeric_limits<T>::epsilon();
    const T nt_lo = static_cast<T>(10) * eps / opts.rtol;
    const T nt_hi = crd::math::sqrt(opts.rtol) < static_cast<T>(0.03) ? crd::math::sqrt(opts.rtol) : static_cast<T>(0.03);
    const T newton_tol = nt_lo > nt_hi ? nt_lo : nt_hi;
    auto atol_i = [&opts](crd::usize i)
    {
        return opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
    };

    cont::Array<T> jac(alloc);
    jac.resize(n * n);
    cont::Array<T> fe(alloc);
    fe.resize(s * n); // explicit stage derivatives
    cont::Array<T> fi(alloc);
    fi.resize(s * n); // implicit stage derivatives
    cont::Array<T> ys(alloc);
    ys.resize(n); // Newton iterate / accepted y_new
    cont::Array<T> base(alloc);
    base.resize(n);
    cont::Array<T> rhsv(alloc);
    rhsv.resize(n);
    cont::Array<T> ytmp(alloc);
    ytmp.resize(n);
    cont::Array<T> scale(alloc);
    scale.resize(n);
    cont::Array<T> fnode(alloc);
    fnode.resize(n); // combined f at a recorded node

    auto fe_row = [&](crd::usize r)
    {
        return cont::Span<T>(fe.data() + r * n, n);
    };
    auto fi_row = [&](crd::usize r)
    {
        return cont::Span<T>(fi.data() + r * n, n);
    };

    auto eval_e = [&fn, &result](T t, cont::ConstSpan<T> yy, cont::Span<T> out)
    {
        const bool ok = fn.rhs_explicit(t, yy, out);
        CRD_ASSERT(ok);
        (void)ok;
        ++result.work.nfev;
    };
    auto eval_i = [&fn, &result](T t, cont::ConstSpan<T> yy, cont::Span<T> out)
    {
        const bool ok = fn.rhs_implicit(t, yy, out);
        CRD_ASSERT(ok);
        (void)ok;
        ++result.work.nfev;
    };

    // J_I = ∂f_I/∂y at (t, yy); fi0 = f_I(t, yy) is the FD base.
    auto build_jacobian = [&](T t, cont::ConstSpan<T> yy, cont::ConstSpan<T> fi0)
    {
        if (fn.has_implicit_jacobian())
        {
            const bool ok = fn.jacobian_implicit(t, yy, cont::Span<T>(jac.data(), n * n));
            CRD_ASSERT(ok);
            (void)ok;
            ++result.work.njev;
            return;
        }
        for (crd::usize i = 0; i < n; ++i)
        {
            ytmp[i] = yy[i];
        }
        const T sqrt_eps = crd::math::sqrt(eps);
        for (crd::usize j = 0; j < n; ++j)
        {
            const T yj = ytmp[j];
            const T mag = std::abs(yj) > static_cast<T>(1) ? std::abs(yj) : static_cast<T>(1);
            const T hj = sqrt_eps * mag;
            ytmp[j] = yj + hj;
            eval_i(t, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(rhsv.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                jac[i * n + j] = (rhsv[i] - fi0[i]) / hj;
            }
            ytmp[j] = yj;
        }
        ++result.work.njev;
    };

    // Simplified Newton for implicit stage i: solves Y = base + γh·f_I(t_s, Y); ys holds start→solution,
    // f_out receives f_I(t_s, Y). Returns true on convergence; counts evals/solves.
    auto stage_newton = [&](T t_s, T dh, cont::Span<T> f_out)
    {
        T dy_norm_old = static_cast<T>(-1);
        for (crd::u32 k = 0; k < detail::imex_newton_maxiter; ++k)
        {
            eval_i(t_s, cont::ConstSpan<T>(ys.data(), n), f_out);
            bool finite = true;
            for (crd::usize i = 0; i < n; ++i)
            {
                finite = finite && std::isfinite(f_out[i]);
            }
            if (!finite)
            {
                return false;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                rhsv[i] = base[i] + dh * f_out[i] - ys[i]; // residual
            }
            lin->solve(cont::Span<T>(rhsv.data(), n));
            ++result.work.nsol;
            T sum = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T q = rhsv[i] / scale[i];
                sum += q * q;
            }
            const T dy_norm = crd::math::sqrt(sum / static_cast<T>(n));
            const bool have_rate = dy_norm_old >= static_cast<T>(0);
            const T rate = have_rate ? dy_norm / dy_norm_old : static_cast<T>(0);
            if (have_rate && rate >= static_cast<T>(1))
            {
                return false;
            }
            for (crd::usize i = 0; i < n; ++i)
            {
                ys[i] += rhsv[i];
            }
            if (dy_norm == static_cast<T>(0) ||
                (have_rate && rate / (static_cast<T>(1) - rate) * dy_norm < newton_tol))
            {
                return true;
            }
            dy_norm_old = dy_norm;
        }
        return false;
    };

    // --- FSAL seed: f_E[0], f_I[0] at (t0, y0); initial combined f0 = fe0 + fi0 ---
    eval_e(t0, cont::ConstSpan<T>(y.data(), n), fe_row(0));
    eval_i(t0, cont::ConstSpan<T>(y.data(), n), fi_row(0));
    for (crd::usize i = 0; i < n; ++i)
    {
        fnode[i] = fe[i] + fi[i];
    }
    if (solution != nullptr)
    {
        solution->reset(n);
        solution->append(t0, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fnode.data(), n));
    }

    // Initial step (scipy/Hairer select_initial_step, order = the error-estimator order p).
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
                d1s += (fnode[i] / sc) * (fnode[i] / sc);
            }
            const T d0 = crd::math::sqrt(d0s / static_cast<T>(n));
            const T d1 = crd::math::sqrt(d1s / static_cast<T>(n));
            T h0_try = (d0 < static_cast<T>(1e-5) || d1 < static_cast<T>(1e-5)) ? static_cast<T>(1e-6)
                                                                                : static_cast<T>(0.01) * d0 / d1;
            h0_try = h0_try < interval_length ? h0_try : interval_length;
            for (crd::usize i = 0; i < n; ++i)
            {
                ytmp[i] = y[i] + h0_try * direction * fnode[i];
            }
            eval_e(t0 + h0_try * direction, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(rhsv.data(), n));
            eval_i(t0 + h0_try * direction, cont::ConstSpan<T>(ytmp.data(), n),
                   cont::Span<T>(base.data(), n)); // base = scratch for f_I1
            T d2s = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
                const T f1 = rhsv[i] + base[i]; // rhsv = f_E1, base = f_I1 (combined f1)
                const T q = (f1 - fnode[i]) / sc;
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
                h1 = crd::math::pow(static_cast<T>(0.01) / dm,
                              static_cast<T>(1) / static_cast<T>(desc.error_estimator_order + 1));
            }
            h_abs = static_cast<T>(100) * h0_try;
            h_abs = h_abs < h1 ? h_abs : h1;
            h_abs = h_abs < interval_length ? h_abs : interval_length;
            h_abs = h_abs < opts.hmax ? h_abs : opts.hmax;
        }
    }

    build_jacobian(t0, cont::ConstSpan<T>(y.data(), n), fi_row(0));
    bool lu_valid = false;
    T lu_dh = static_cast<T>(0);

    ElementaryController<T> controller;
    controller.exponent = static_cast<T>(-1) / static_cast<T>(desc.error_estimator_order + 1);

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
        if (h_abs > opts.hmax)
        {
            h_abs = opts.hmax;
        }

        bool step_accepted = false;
        T t_new = t;
        bool current_jac = false;

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
            const T dh = gamma * h;

            for (crd::usize i = 0; i < n; ++i)
            {
                scale[i] = atol_i(i) + std::abs(y[i]) * opts.rtol;
            }

            bool converged = false;
            while (!converged)
            {
                if (!lu_valid || lu_dh != dh)
                {
                    const bool ok = lin->factor_iteration_matrix(dh, cont::ConstSpan<T>(jac.data(), n * n), n);
                    ++result.work.nlu;
                    lu_valid = ok;
                    lu_dh = dh;
                    if (!ok)
                    {
                        break;
                    }
                }
                converged = true;
                for (crd::usize stg = 1; stg < s && converged; ++stg)
                {
                    // base = y + h·Σ_{j<stg}(ae·fe_j + ai·fi_j); start iterate = base (trivial predictor).
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        T acc = static_cast<T>(0);
                        for (crd::usize j = 0; j < stg; ++j)
                        {
                            acc += static_cast<T>(desc.ae[stg * s + j]) * fe[j * n + i] +
                                   static_cast<T>(desc.ai[stg * s + j]) * fi[j * n + i];
                        }
                        base[i] = y[i] + h * acc;
                        ys[i] = base[i];
                    }
                    const T t_s = t + static_cast<T>(desc.c[stg]) * h;
                    converged = stage_newton(t_s, dh, fi_row(stg));
                    if (converged)
                    {
                        eval_e(t_s, cont::ConstSpan<T>(ys.data(), n), fe_row(stg));
                    }
                }
                if (!converged)
                {
                    if (current_jac)
                    {
                        break;
                    }
                    build_jacobian(t, cont::ConstSpan<T>(y.data(), n), fi_row(0));
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

            // y_new = y + h·Σ b_i·(fe_i + fi_i); error = h·Σ (b_i − d_i)·(fe_i + fi_i).
            T esum = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                T sb = static_cast<T>(0);
                T se = static_cast<T>(0);
                for (crd::usize j = 0; j < s; ++j)
                {
                    const T fij = fe[j * n + i] + fi[j * n + i];
                    sb += static_cast<T>(desc.b[j]) * fij;
                    se += (static_cast<T>(desc.b[j]) - static_cast<T>(desc.d[j])) * fij;
                }
                ys[i] = y[i] + h * sb;
                const T e_i = h * se;
                const T sk =
                    atol_i(i) + opts.rtol * (std::abs(y[i]) > std::abs(ys[i]) ? std::abs(y[i]) : std::abs(ys[i]));
                esum += (e_i / sk) * (e_i / sk);
            }
            const T error_norm = crd::math::sqrt(esum / static_cast<T>(n));

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
                // FSAL: stage 0 of the next step is at (t_new, y_new) — evaluate both halves here (also the
                // dense-output node f).
                eval_e(t, cont::ConstSpan<T>(y.data(), n), fe_row(0));
                eval_i(t, cont::ConstSpan<T>(y.data(), n), fi_row(0));
                if (solution != nullptr)
                {
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        fnode[i] = fe[i] + fi[i];
                    }
                    solution->append(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fnode.data(), n));
                }
            }
            else
            {
                ++result.work.nreject;
            }
            h_abs *= factor;
        }
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
