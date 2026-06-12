#pragma once

// radau.hpp — Phase 3.1.6 v9-e: Radau IIA order 5 (= RADAU5) with scipy `solve_ivp` EXACT semantics
// (scipy 1.17.1 `_ivp/radau.py` read verbatim; constants EXTRACTED from the module at 17 digits):
//   • the 3-stage collocation system solved in the eigenbasis of A⁻¹ — ONE real solve with
//     (μ_R/h·I − J) and ONE complex solve with (μ_C/h·I − J) per Newton iteration (the hesap-dense
//     COMPLEX LU is the named v9-e consumer edge),
//   • `solve_collocation_system` verbatim (6-iteration Newton on W = TI·Z, the rate predicates),
//   • the embedded 3rd-order error with the LU_real-stabilized estimate and the REJECTED-step
//     re-stabilization (f(t, y + error) — one extra eval, counted like scipy's),
//   • the Gustafsson PREDICTIVE controller (`predict_factor`, Hairer IV.8) + the keep-LU rule
//     (factor < 1.2 and no Jacobian recompute ⇒ factor = 1, factorizations kept),
//   • the collocation-polynomial dense output (Q = Zᵀ·P) — ALSO the Z0 warm start for the next step
//     (scipy: Z0 = sol(t + h·C) − y), so it is part of the trajectory contract, not an extra.
// Jacobian policy as in bdf.hpp (analytic = the trajectory-exact configuration; plain-FD fallback named).
// ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/dense/lu.hpp>
#include <crd/hesap/dense/matrix.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocator.hpp>

#include <cmath>
#include <limits>

namespace crd::hesap::ode
{

namespace detail
{

// Radau IIA(5) constants (printed from scipy at 17 significant digits; S6 = sqrt(6)).
inline constexpr crd::f64 radau_c[3] = {0.15505102572168222, 0.64494897427831777, 1.0};
inline constexpr crd::f64 radau_e[3] = {-10.048809399827414, 1.3821427331607481, -1.0 / 3.0};
inline constexpr crd::f64 radau_mu_real = 3.6378342527444958;
inline constexpr crd::f64 radau_mu_complex_re = 2.6810828736277523;
inline constexpr crd::f64 radau_mu_complex_im = -3.050430199247411;
inline constexpr crd::f64 radau_t[9] = {0.09443876248897524,
                                        -0.14125529502095421,
                                        0.03002919410514742,
                                        0.25021312296533332,
                                        0.20412935229379994,
                                        -0.38294211275726192,
                                        1.0,
                                        1.0,
                                        0.0};
inline constexpr crd::f64 radau_ti[9] = {4.17871859155190428,  0.32768282076106237,  0.52337644549944951,
                                         -4.17871859155190428, -0.32768282076106237, 0.47662355450055044,
                                         0.50287263494578682,  -2.57192694985560522, 0.59603920482822492};
inline constexpr crd::f64 radau_p[9] = {10.048809399827414,  -25.629591447076638, 15.580782047249224,
                                        -1.3821427331607481, 10.296258113743303,  -8.9141153805825564,
                                        1.0 / 3.0,           -8.0 / 3.0,          10.0 / 3.0};
inline constexpr crd::u32 radau_newton_maxiter = 6;

} // namespace detail

// Radau IIA(5). `solution` recording is free (f_new is computed per accepted step anyway).
template <typename T>
[[nodiscard]] OdeResult<T> integrate_radau(const OdeFunction<T>& fn, T t0, T t1, crd::containers::Span<T> y,
                                           const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                           OdeSolution<T>* solution = nullptr)
{
    namespace cont = crd::containers;
    using Cx = crd::hesap::Complex<T>;
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

    const T direction = (t1 > t0) ? static_cast<T>(1) : static_cast<T>(-1);
    const T interval_length = std::abs(t1 - t0);
    const T max_step = opts.hmax;
    const T eps = std::numeric_limits<T>::epsilon();
    const T nt_lo = static_cast<T>(10) * eps / opts.rtol;
    const T nt_hi = std::sqrt(opts.rtol) < static_cast<T>(0.03) ? std::sqrt(opts.rtol) : static_cast<T>(0.03);
    const T newton_tol = nt_lo > nt_hi ? nt_lo : nt_hi;

    auto atol_i = [&opts](crd::usize i)
    {
        return opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
    };

    // Workspace.
    cont::Array<T> jac(alloc);
    jac.resize(n * n);
    cont::Array<T> fvec(alloc);
    fvec.resize(n);
    cont::Array<T> ytmp(alloc);
    ytmp.resize(n);
    cont::Array<T> zbuf(alloc); // Z (3 x n)
    zbuf.resize(3 * n);
    cont::Array<T> z0buf(alloc); // Z0 (3 x n)
    z0buf.resize(3 * n);
    cont::Array<T> wbuf(alloc); // W (3 x n)
    wbuf.resize(3 * n);
    cont::Array<T> fst(alloc); // F (3 x n)
    fst.resize(3 * n);
    cont::Array<T> dw(alloc); // dW (3 x n)
    dw.resize(3 * n);
    cont::Array<T> scale(alloc);
    scale.resize(n);
    cont::Array<T> y_new(alloc);
    y_new.resize(n);
    cont::Array<T> err(alloc);
    err.resize(n);
    cont::Array<Cx> fcx(alloc); // complex Newton rhs / solution
    fcx.resize(n);
    cont::Array<T> qbuf(alloc); // dense-output Q (n x 3) of the LAST accepted step
    qbuf.resize(n * 3);
    cont::Array<T> y_old_buf(alloc);
    y_old_buf.resize(n);

    dense::Matrix<T, dense::Layout::RowMajor> m_real(alloc, n, n);
    dense::LU<T, dense::Layout::RowMajor> lu_real(alloc, n);
    dense::Matrix<Cx, dense::Layout::RowMajor> m_cx(alloc, n, n);
    dense::LU<Cx, dense::Layout::RowMajor> lu_cx(alloc, n);
    bool lu_valid = false;

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
        // Plain forward differences off the supplied f(t, y) (named non-num_jac divergence).
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
            eval(t, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(err.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                jac[i * n + j] = (err[i] - f_at[i]) / hj;
            }
            ytmp[j] = yj;
        }
        ++result.work.njev;
    };
    auto factor_pair = [&](T h)
    {
        const T cr = static_cast<T>(detail::radau_mu_real) / h;
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                m_real.at(i, j) = ((i == j) ? cr : static_cast<T>(0)) - jac[i * n + j];
            }
        }
        dense::factor_lu(lu_real, m_real);
        ++result.work.nlu;
        const Cx cc(static_cast<T>(detail::radau_mu_complex_re) / h, static_cast<T>(detail::radau_mu_complex_im) / h);
        for (crd::usize i = 0; i < n; ++i)
        {
            for (crd::usize j = 0; j < n; ++j)
            {
                Cx v(-jac[i * n + j], static_cast<T>(0));
                if (i == j)
                {
                    v = Cx(cc.re - jac[i * n + j], cc.im);
                }
                m_cx.at(i, j) = v;
            }
        }
        dense::factor_lu(lu_cx, m_cx);
        ++result.work.nlu;
        lu_valid = (lu_real.info() == 0) && (lu_cx.info() == 0);
        return lu_valid;
    };
    auto rms3n = [&](const T* v)
    {
        T sum = static_cast<T>(0);
        for (crd::usize r = 0; r < 3; ++r)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                const T q = v[r * n + i] / scale[i];
                sum += q * q;
            }
        }
        return std::sqrt(sum / static_cast<T>(3 * n));
    };

    // f0 + initial step (scipy Radau: select_initial_step with order = 3) + initial Jacobian.
    eval(t0, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(fvec.data(), n));
    if (solution != nullptr)
    {
        solution->reset(n);
        solution->append(t0, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fvec.data(), n));
    }

    T h_abs;
    if (opts.h0 > static_cast<T>(0))
    {
        h_abs = std::abs(opts.h0);
        if (h_abs > interval_length)
        {
            h_abs = interval_length;
        }
    }
    else
    {
        T d0s = static_cast<T>(0);
        T d1s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
            const T q0 = y[i] / sc;
            const T q1 = fvec[i] / sc;
            d0s += q0 * q0;
            d1s += q1 * q1;
        }
        const T d0 = std::sqrt(d0s / static_cast<T>(n));
        const T d1 = std::sqrt(d1s / static_cast<T>(n));
        T h0_try = (d0 < static_cast<T>(1e-5) || d1 < static_cast<T>(1e-5)) ? static_cast<T>(1e-6)
                                                                            : static_cast<T>(0.01) * d0 / d1;
        h0_try = h0_try < interval_length ? h0_try : interval_length;
        for (crd::usize i = 0; i < n; ++i)
        {
            ytmp[i] = y[i] + h0_try * direction * fvec[i];
        }
        eval(t0 + h0_try * direction, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(err.data(), n));
        T d2s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
            const T q = (err[i] - fvec[i]) / sc;
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
            h1 = std::pow(static_cast<T>(0.01) / dm, static_cast<T>(1) / static_cast<T>(4)); // order 3 ⇒ 1/(3+1)
        }
        h_abs = static_cast<T>(100) * h0_try;
        h_abs = h_abs < h1 ? h_abs : h1;
        h_abs = h_abs < interval_length ? h_abs : interval_length;
        h_abs = h_abs < max_step ? h_abs : max_step;
    }

    build_jacobian(t0, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fvec.data(), n));
    bool current_jac = true;
    bool have_sol = false; // dense output of the previous step exists (Z0 warm start)
    T sol_t_old = t0;
    T sol_h = static_cast<T>(0);
    T h_abs_old_stored = static_cast<T>(-1); // scipy h_abs_old (None ⇒ -1)
    T error_norm_old_stored = static_cast<T>(-1);

    auto predict_factor = [](T h_abs_cur, T h_abs_old, T error_norm, T error_norm_old)
    {
        T multiplier = static_cast<T>(1);
        if (error_norm_old >= static_cast<T>(0) && h_abs_old >= static_cast<T>(0) && error_norm != static_cast<T>(0))
        {
            multiplier = h_abs_cur / h_abs_old * std::pow(error_norm_old / error_norm, static_cast<T>(0.25));
        }
        const T m1 = multiplier < static_cast<T>(1) ? multiplier : static_cast<T>(1);
        return (error_norm == static_cast<T>(0)) ? std::numeric_limits<T>::infinity()
                                                 : m1 * std::pow(error_norm, static_cast<T>(-0.25));
    };

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
        const T h_abs_at_entry = h_abs; // scipy's self.h_abs — h_abs_old at the accept tail stores THIS
        T h_abs_old = h_abs_old_stored;
        T error_norm_old = error_norm_old_stored;
        if (h_abs > max_step)
        {
            h_abs = max_step;
            h_abs_old = static_cast<T>(-1);
            error_norm_old = static_cast<T>(-1);
        }
        else if (h_abs < min_step)
        {
            h_abs = min_step;
            h_abs_old = static_cast<T>(-1);
            error_norm_old = static_cast<T>(-1);
        }

        bool rejected = false;
        bool step_accepted = false;
        T t_new = t;
        T error_norm = static_cast<T>(0);
        T safety = static_cast<T>(0);
        crd::u32 n_iter = 0;
        T rate = static_cast<T>(0);

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

            // Z0: zeros on the first step, else the previous collocation polynomial at t + h·C minus y.
            if (!have_sol)
            {
                for (crd::usize i = 0; i < 3 * n; ++i)
                {
                    z0buf[i] = static_cast<T>(0);
                }
            }
            else
            {
                for (crd::usize s = 0; s < 3; ++s)
                {
                    const T x = (t + h * static_cast<T>(detail::radau_c[s]) - sol_t_old) / sol_h;
                    const T p1 = x;
                    const T p2 = x * x;
                    const T p3 = p2 * x;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        z0buf[s * n + i] =
                            y_old_buf[i] + qbuf[i * 3 + 0] * p1 + qbuf[i * 3 + 1] * p2 + qbuf[i * 3 + 2] * p3 - y[i];
                    }
                }
            }

            for (crd::usize i = 0; i < n; ++i)
            {
                scale[i] = atol_i(i) + std::abs(y[i]) * opts.rtol;
            }

            bool converged = false;
            while (!converged)
            {
                if (!lu_valid)
                {
                    if (!factor_pair(h))
                    {
                        break;
                    }
                }

                // solve_collocation_system (scipy, verbatim).
                const T m_real_c = static_cast<T>(detail::radau_mu_real) / h;
                const Cx m_cx_c(static_cast<T>(detail::radau_mu_complex_re) / h,
                                static_cast<T>(detail::radau_mu_complex_im) / h);
                for (crd::usize i = 0; i < 3 * n; ++i)
                {
                    zbuf[i] = z0buf[i];
                }
                // W = TI · Z0
                for (crd::usize r = 0; r < 3; ++r)
                {
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        wbuf[r * n + i] = static_cast<T>(detail::radau_ti[r * 3 + 0]) * z0buf[0 * n + i] +
                                          static_cast<T>(detail::radau_ti[r * 3 + 1]) * z0buf[1 * n + i] +
                                          static_cast<T>(detail::radau_ti[r * 3 + 2]) * z0buf[2 * n + i];
                    }
                }
                T dw_norm_old = static_cast<T>(-1);
                converged = false;
                rate = static_cast<T>(0);
                for (crd::u32 k = 0; k < detail::radau_newton_maxiter; ++k)
                {
                    n_iter = k + 1;
                    bool finite = true;
                    for (crd::usize s = 0; s < 3; ++s)
                    {
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            ytmp[i] = y[i] + zbuf[s * n + i];
                        }
                        eval(t + static_cast<T>(detail::radau_c[s]) * h, cont::ConstSpan<T>(ytmp.data(), n),
                             cont::Span<T>(fst.data() + s * n, n));
                    }
                    for (crd::usize i = 0; i < 3 * n && finite; ++i)
                    {
                        finite = std::isfinite(fst[i]);
                    }
                    if (!finite)
                    {
                        break;
                    }
                    // f_real = Fᵀ·TI_REAL − M_real·W[0]; f_complex = Fᵀ·TI_COMPLEX − M_complex·(W1 + iW2)
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        err[i] = static_cast<T>(detail::radau_ti[0]) * fst[0 * n + i] +
                                 static_cast<T>(detail::radau_ti[1]) * fst[1 * n + i] +
                                 static_cast<T>(detail::radau_ti[2]) * fst[2 * n + i] - m_real_c * wbuf[0 * n + i];
                        const T fr = static_cast<T>(detail::radau_ti[3]) * fst[0 * n + i] +
                                     static_cast<T>(detail::radau_ti[4]) * fst[1 * n + i] +
                                     static_cast<T>(detail::radau_ti[5]) * fst[2 * n + i];
                        const T fi = static_cast<T>(detail::radau_ti[6]) * fst[0 * n + i] +
                                     static_cast<T>(detail::radau_ti[7]) * fst[1 * n + i] +
                                     static_cast<T>(detail::radau_ti[8]) * fst[2 * n + i];
                        const Cx w12(wbuf[1 * n + i], wbuf[2 * n + i]);
                        const Cx prod = m_cx_c * w12;
                        fcx[i] = Cx(fr - prod.re, fi - prod.im);
                    }
                    dense::solve_lu(lu_real, cont::Span<T>(err.data(), n));
                    dense::solve_lu(lu_cx, cont::Span<Cx>(fcx.data(), n));
                    ++result.work.nsol;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        dw[0 * n + i] = err[i];
                        dw[1 * n + i] = fcx[i].re;
                        dw[2 * n + i] = fcx[i].im;
                    }
                    const T dw_norm = rms3n(dw.data());
                    const bool have_rate = dw_norm_old >= static_cast<T>(0);
                    rate = have_rate ? dw_norm / dw_norm_old : static_cast<T>(0);
                    if (have_rate &&
                        (rate >= static_cast<T>(1) || std::pow(rate, static_cast<T>(detail::radau_newton_maxiter - k)) /
                                                              (static_cast<T>(1) - rate) * dw_norm >
                                                          newton_tol))
                    {
                        break;
                    }
                    for (crd::usize i = 0; i < 3 * n; ++i)
                    {
                        wbuf[i] += dw[i];
                    }
                    // Z = T · W
                    for (crd::usize r = 0; r < 3; ++r)
                    {
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            zbuf[r * n + i] = static_cast<T>(detail::radau_t[r * 3 + 0]) * wbuf[0 * n + i] +
                                              static_cast<T>(detail::radau_t[r * 3 + 1]) * wbuf[1 * n + i] +
                                              static_cast<T>(detail::radau_t[r * 3 + 2]) * wbuf[2 * n + i];
                        }
                    }
                    if (dw_norm == static_cast<T>(0) ||
                        (have_rate && rate / (static_cast<T>(1) - rate) * dw_norm < newton_tol))
                    {
                        converged = true;
                        break;
                    }
                    dw_norm_old = dw_norm;
                }

                if (!converged)
                {
                    if (current_jac)
                    {
                        break;
                    }
                    build_jacobian(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fvec.data(), n));
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

            // y_new = y + Z[2]; error = LU_real⁻¹(f + Zᵀ·E/h), the stabilized 3rd-order estimate.
            for (crd::usize i = 0; i < n; ++i)
            {
                y_new[i] = y[i] + zbuf[2 * n + i];
                err[i] = fvec[i] + (static_cast<T>(detail::radau_e[0]) * zbuf[0 * n + i] +
                                    static_cast<T>(detail::radau_e[1]) * zbuf[1 * n + i] +
                                    static_cast<T>(detail::radau_e[2]) * zbuf[2 * n + i]) /
                                       h;
            }
            dense::solve_lu(lu_real, cont::Span<T>(err.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                scale[i] =
                    atol_i(i) + (std::abs(y[i]) > std::abs(y_new[i]) ? std::abs(y[i]) : std::abs(y_new[i])) * opts.rtol;
            }
            T esum = static_cast<T>(0);
            for (crd::usize i = 0; i < n; ++i)
            {
                const T q = err[i] / scale[i];
                esum += q * q;
            }
            error_norm = std::sqrt(esum / static_cast<T>(n));
            safety = static_cast<T>(0.9) * static_cast<T>(2 * detail::radau_newton_maxiter + 1) /
                     static_cast<T>(2 * detail::radau_newton_maxiter + n_iter);

            if (rejected && error_norm > static_cast<T>(1))
            {
                // Stabilize once more through f(t, y + error) (scipy's rejected-retry refinement).
                for (crd::usize i = 0; i < n; ++i)
                {
                    ytmp[i] = y[i] + err[i];
                }
                eval(t, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(err.data(), n));
                for (crd::usize i = 0; i < n; ++i)
                {
                    err[i] += (static_cast<T>(detail::radau_e[0]) * zbuf[0 * n + i] +
                               static_cast<T>(detail::radau_e[1]) * zbuf[1 * n + i] +
                               static_cast<T>(detail::radau_e[2]) * zbuf[2 * n + i]) /
                              h;
                }
                dense::solve_lu(lu_real, cont::Span<T>(err.data(), n));
                esum = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    const T q = err[i] / scale[i];
                    esum += q * q;
                }
                error_norm = std::sqrt(esum / static_cast<T>(n));
            }

            if (error_norm > static_cast<T>(1))
            {
                T factor = predict_factor(h_abs, h_abs_old, error_norm, error_norm_old);
                T scaled = safety * factor;
                if (scaled < static_cast<T>(0.2))
                {
                    scaled = static_cast<T>(0.2);
                }
                h_abs *= scaled;
                lu_valid = false;
                rejected = true;
                ++result.work.nreject;
            }
            else
            {
                step_accepted = true;
                ++result.work.naccept;
            }
        }

        // Accepted (scipy tail): recompute_jac, the predictive factor, the keep-LU rule.
        const bool recompute_jac = n_iter > 2 && rate > static_cast<T>(1e-3);
        T factor = predict_factor(h_abs, h_abs_old, error_norm, error_norm_old);
        const T sf = safety * factor;
        factor = sf < static_cast<T>(10) ? sf : static_cast<T>(10);
        if (!recompute_jac && factor < static_cast<T>(1.2))
        {
            factor = static_cast<T>(1);
        }
        else
        {
            lu_valid = false;
        }

        eval(t_new, cont::ConstSpan<T>(y_new.data(), n), cont::Span<T>(ytmp.data(), n)); // f_new
        if (recompute_jac)
        {
            build_jacobian(t_new, cont::ConstSpan<T>(y_new.data(), n), cont::ConstSpan<T>(ytmp.data(), n));
            current_jac = true;
        }
        else
        {
            current_jac = false;
        }

        h_abs_old_stored = h_abs_at_entry; // scipy: self.h_abs_old = self.h_abs (the PRE-step value)
        error_norm_old_stored = error_norm;

        // Dense output Q = Zᵀ·P (n x 3) + the warm-start state.
        for (crd::usize i = 0; i < n; ++i)
        {
            y_old_buf[i] = y[i];
            for (crd::usize col = 0; col < 3; ++col)
            {
                qbuf[i * 3 + col] = zbuf[0 * n + i] * static_cast<T>(detail::radau_p[0 * 3 + col]) +
                                    zbuf[1 * n + i] * static_cast<T>(detail::radau_p[1 * 3 + col]) +
                                    zbuf[2 * n + i] * static_cast<T>(detail::radau_p[2 * 3 + col]);
            }
        }
        sol_t_old = t;
        sol_h = t_new - t;
        have_sol = true;

        t = t_new;
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = y_new[i];
            fvec[i] = ytmp[i]; // f_new
        }
        h_abs *= factor;

        if (solution != nullptr)
        {
            solution->append(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fvec.data(), n));
        }
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
