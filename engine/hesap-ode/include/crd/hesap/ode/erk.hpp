#pragma once

// erk.hpp — Phase 3.1.6 v9-b: embedded explicit Runge-Kutta with scipy `solve_ivp` EXACT semantics
// (scipy 1.17.1 `_ivp/rk.py` + `_ivp/common.py`, read verbatim — the v7 trajectory-exactness playbook):
//   • the stage loop `rk_step` (K[0] = f; stages 1..s−1; y_new = y + h·Bᵀ·K[:s]; f_new evaluated EVERY
//     attempt and stored in K[s] — eval counts match scipy's to the call),
//   • Hairer automatic initial step (`select_initial_step`, order = the ERROR-estimator order),
//   • the step loop verbatim: min_step = 10·|nextafter(t, dir·inf) − t|, t_bound clamping, scale =
//     atol + max(|y|, |y_new|)·rtol, RMS error norm, the elementary controller (accept err < 1 strictly,
//     post-rejection growth cap — controller.hpp's ElementaryController IS this code),
//   • DOP853's special combined 5th/3rd error norm (|h|·‖err5‖² / √((‖err5‖² + 0.01·‖err3‖²)·n)).
//
// METHODS: RK23 (Bogacki-Shampine 3(2)) · RK45 (Dormand-Prince 5(4)) · DOP853 (Hairer 8(5,3)) — tableaus
// GENERATED from the installed scipy by scripts/gen_erk_tableaus.py (extraction beats transcription) ·
// Cash-Karp 5(4) (fractions written as exact f64 expressions) · Tsit5 (Tsitouras 2011, the FSAL form that
// maps onto the same 6-stage + f_new frame as RK45) — both gated by order-slope certificates.
//
// Dense output: the driver records (t, y, f) nodes into an optional OdeSolution (Hermite fallback per the
// ADR-0091 contract; native interpolants are a named follow-up). Events (v9-c): scipy semantics over the
// recorded interpolant. ADR-0091.

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/ode/controller.hpp>
#include <crd/hesap/ode/dense_output.hpp>
#include <crd/hesap/ode/detail/brentq.hpp>
#include <crd/hesap/ode/detail/erk_tableaus.hpp>
#include <crd/hesap/ode/events.hpp>
#include <crd/hesap/ode/ode_function.hpp>
#include <crd/hesap/ode/ode_types.hpp>
#include <crd/hesap/ode/solution.hpp>
#include <crd/memory/allocator.hpp>

#include <crd/math/cmath.hpp>
#include <limits>

namespace crd::hesap::ode
{

enum class ErkMethod : crd::u8
{
    Rk23 = 0,     // order 3, error estimator 2 (scipy RK23)
    Rk45 = 1,     // order 5, error estimator 4 (scipy RK45 — the solve_ivp default)
    CashKarp = 2, // order 5, error estimator 4
    Tsit5 = 3,    // order 5, error estimator 4 (Tsitouras 2011)
    Dop853 = 4,   // order 8, error estimator 7 (scipy DOP853; combined 5th/3rd error)
};

namespace detail
{

// Cash-Karp 5(4) — exact rational coefficients written as f64 expressions (no transcription rounding).
inline constexpr crd::f64 ck_c[6] = {0.0, 1.0 / 5.0, 3.0 / 10.0, 3.0 / 5.0, 1.0, 7.0 / 8.0};
inline constexpr crd::f64 ck_a[36] = {0.0,
                                      0.0,
                                      0.0,
                                      0.0,
                                      0.0,
                                      0.0, //
                                      1.0 / 5.0,
                                      0.0,
                                      0.0,
                                      0.0,
                                      0.0,
                                      0.0, //
                                      3.0 / 40.0,
                                      9.0 / 40.0,
                                      0.0,
                                      0.0,
                                      0.0,
                                      0.0, //
                                      3.0 / 10.0,
                                      -9.0 / 10.0,
                                      6.0 / 5.0,
                                      0.0,
                                      0.0,
                                      0.0, //
                                      -11.0 / 54.0,
                                      5.0 / 2.0,
                                      -70.0 / 27.0,
                                      35.0 / 27.0,
                                      0.0,
                                      0.0, //
                                      1631.0 / 55296.0,
                                      175.0 / 512.0,
                                      575.0 / 13824.0,
                                      44275.0 / 110592.0,
                                      253.0 / 4096.0,
                                      0.0};
inline constexpr crd::f64 ck_b[6] = {37.0 / 378.0, 0.0, 250.0 / 621.0, 125.0 / 594.0, 0.0, 512.0 / 1771.0};
// E = b(5th) − b*(4th); the f_new slot is 0 (Cash-Karp's error does not use f_new).
inline constexpr crd::f64 ck_e[7] = {37.0 / 378.0 - 2825.0 / 27648.0,
                                     0.0,
                                     250.0 / 621.0 - 18575.0 / 48384.0,
                                     125.0 / 594.0 - 13525.0 / 55296.0,
                                     -277.0 / 14336.0,
                                     512.0 / 1771.0 - 1.0 / 4.0,
                                     0.0};

// Tsit5 (Tsitouras 2011, Table 1) in the scipy 6-stage + f_new frame (FSAL: B is the a7 row, so
// f_new = k7; E = the btilde error weights, btilde7 ≠ 0). Order-5 slope gate certifies the constants.
inline constexpr crd::f64 tsit5_c[6] = {0.0, 0.161, 0.327, 0.9, 0.9800255409045097, 1.0};
inline constexpr crd::f64 tsit5_a[36] = {0.0,
                                         0.0,
                                         0.0,
                                         0.0,
                                         0.0,
                                         0.0, //
                                         0.161,
                                         0.0,
                                         0.0,
                                         0.0,
                                         0.0,
                                         0.0, //
                                         -0.008480655492356989,
                                         0.335480655492357,
                                         0.0,
                                         0.0,
                                         0.0,
                                         0.0, //
                                         2.8971530571054935,
                                         -6.359448489975075,
                                         4.3622954328695815,
                                         0.0,
                                         0.0,
                                         0.0, //
                                         5.325864828439257,
                                         -11.748883564062828,
                                         7.4955393428898365,
                                         -0.09249506636175525,
                                         0.0,
                                         0.0, //
                                         5.86145544294642,
                                         -12.92096931784711,
                                         8.159367898576159,
                                         -0.071584973281401,
                                         -0.028269050394068383,
                                         0.0};
inline constexpr crd::f64 tsit5_b[6] = {
    0.09646076681806523, 0.01, 0.4798896504144996, 1.379008574103742, -3.290069515436081, 2.324710524099774};
inline constexpr crd::f64 tsit5_e[7] = {-0.00178001105222577714, -0.0008164344596567469, 0.007880878010261995,
                                        -0.1447110071732629,     0.5823571654525552,     -0.45808210592918697,
                                        0.015151515151515152};

struct ErkDesc
{
    const crd::f64* a;  // stages × stages, row-major
    const crd::f64* b;  // stages
    const crd::f64* c;  // stages
    const crd::f64* e;  // stages+1 (standard error mode; null for DOP853)
    const crd::f64* e3; // stages+1 (DOP853 mode)
    const crd::f64* e5; // stages+1 (DOP853 mode)
    crd::usize stages;
    crd::i32 error_estimator_order;
};

[[nodiscard]] inline ErkDesc erk_desc(ErkMethod m) noexcept
{
    switch (m)
    {
        case ErkMethod::Rk23:
            return {rk23_a, rk23_b, rk23_c, rk23_e, nullptr, nullptr, 3, 2};
        case ErkMethod::Rk45:
            return {rk45_a, rk45_b, rk45_c, rk45_e, nullptr, nullptr, 6, 4};
        case ErkMethod::CashKarp:
            return {ck_a, ck_b, ck_c, ck_e, nullptr, nullptr, 6, 4};
        case ErkMethod::Tsit5:
            return {tsit5_a, tsit5_b, tsit5_c, tsit5_e, nullptr, nullptr, 6, 4};
        case ErkMethod::Dop853:
        default:
            return {dop853_a, dop853_b, dop853_c, nullptr, dop853_e3, dop853_e5, 12, 7};
    }
}

// scipy common.py norm(): RMS.
template <typename T> [[nodiscard]] T rms_norm(crd::containers::ConstSpan<T> x) noexcept
{
    if (x.empty())
    {
        return static_cast<T>(0);
    }
    T sum = static_cast<T>(0);
    for (crd::usize i = 0; i < x.size(); ++i)
    {
        sum += x[i] * x[i];
    }
    return crd::math::sqrt(sum / static_cast<T>(x.size()));
}

} // namespace detail

// Embedded adaptive explicit RK with scipy-exact semantics. `y` is in-out; `solution` (optional) records
// the trajectory for dense output / t_eval; `events` (optional) are checked per accepted step (v9-c).
template <typename T>
[[nodiscard]] OdeResult<T> integrate_erk(const OdeFunction<T>& fn, T t0, T t1, crd::containers::Span<T> y,
                                         const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                         ErkMethod method = ErkMethod::Rk45, OdeSolution<T>* solution = nullptr,
                                         crd::containers::ConstSpan<OdeEvent<T>*> events = {})
{
    namespace cont = crd::containers;
    const crd::usize n = fn.dim();
    CRD_ASSERT(y.size() == n);
    CRD_ASSERT(alloc != nullptr);

    OdeResult<T> result;
    result.t = t0;

    if (!std::isfinite(t0) || !std::isfinite(t1))
    {
        return result; // InvalidInput
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

    const detail::ErkDesc desc = detail::erk_desc(method);
    const crd::usize s = desc.stages;
    const T direction = (t1 > t0) ? static_cast<T>(1) : static_cast<T>(-1);
    const T interval_length = std::abs(t1 - t0);
    const T max_step = opts.hmax;

    ElementaryController<T> controller;
    controller.exponent = static_cast<T>(-1) / static_cast<T>(desc.error_estimator_order + 1);

    // Workspace: K ((s+1) rows × n), y_new, dy, g values for events.
    cont::Array<T> kbuf(alloc);
    kbuf.resize((s + 1) * n);
    cont::Array<T> ynew_buf(alloc);
    ynew_buf.resize(n);
    cont::Array<T> ytmp_buf(alloc);
    ytmp_buf.resize(n);
    const cont::Span<T> y_new(ynew_buf.data(), n);
    const cont::Span<T> ytmp(ytmp_buf.data(), n);
    auto krow = [&kbuf, n](crd::usize r)
    {
        return cont::Span<T>(kbuf.data() + r * n, n);
    };
    auto krow_c = [&kbuf, n](crd::usize r)
    {
        return cont::ConstSpan<T>(kbuf.data() + r * n, n);
    };

    auto eval = [&fn, &result](T t, cont::ConstSpan<T> yy, cont::Span<T> out)
    {
        fn.rhs(t, yy, out);
        ++result.work.nfev;
    };

    // WRMS-free scipy scale + RMS error norm helpers (atol vector honored).
    auto scale_at = [&opts](crd::usize i, T y0i, T y1i)
    {
        const T a = opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
        const T m = std::abs(y0i) > std::abs(y1i) ? std::abs(y0i) : std::abs(y1i);
        return a + opts.rtol * m;
    };

    // f0:
    eval(t0, cont::ConstSpan<T>(y.data(), n), krow(0));

    if (solution != nullptr)
    {
        solution->reset(n);
        solution->append(t0, cont::ConstSpan<T>(y.data(), n), krow_c(0));
    }

    // Initial event values.
    cont::Array<T> g_prev(alloc);
    g_prev.resize(events.size());
    for (crd::usize ev = 0; ev < events.size(); ++ev)
    {
        g_prev[ev] = events[ev]->value(t0, cont::ConstSpan<T>(y.data(), n));
    }

    // --- Initial step: scipy select_initial_step (Hairer), order = error-estimator order ---
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
        T d0_sum = static_cast<T>(0);
        T d1_sum = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T sc = (opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i]) + std::abs(y[i]) * opts.rtol;
            const T q0 = y[i] / sc;
            const T q1 = kbuf[i] / sc;
            d0_sum += q0 * q0;
            d1_sum += q1 * q1;
        }
        const T d0 = crd::math::sqrt(d0_sum / static_cast<T>(n));
        const T d1 = crd::math::sqrt(d1_sum / static_cast<T>(n));
        T h0_try = (d0 < static_cast<T>(1e-5) || d1 < static_cast<T>(1e-5)) ? static_cast<T>(1e-6)
                                                                            : static_cast<T>(0.01) * d0 / d1;
        h0_try = h0_try < interval_length ? h0_try : interval_length;
        for (crd::usize i = 0; i < n; ++i)
        {
            ytmp[i] = y[i] + h0_try * direction * kbuf[i];
        }
        eval(t0 + h0_try * direction, cont::ConstSpan<T>(ytmp.data(), n), y_new); // y_new as f1 scratch
        T d2_sum = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T sc = (opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i]) + std::abs(y[i]) * opts.rtol;
            const T q = (y_new[i] - kbuf[i]) / sc;
            d2_sum += q * q;
        }
        const T d2 = crd::math::sqrt(d2_sum / static_cast<T>(n)) / h0_try;
        T h1;
        if (d1 <= static_cast<T>(1e-15) && d2 <= static_cast<T>(1e-15))
        {
            h1 = static_cast<T>(1e-6) > h0_try * static_cast<T>(1e-3) ? static_cast<T>(1e-6)
                                                                      : h0_try * static_cast<T>(1e-3);
        }
        else
        {
            const T dm = d1 > d2 ? d1 : d2;
            h1 =
                crd::math::pow(static_cast<T>(0.01) / dm, static_cast<T>(1) / static_cast<T>(desc.error_estimator_order + 1));
        }
        h_abs = static_cast<T>(100) * h0_try;
        h_abs = h_abs < h1 ? h_abs : h1;
        h_abs = h_abs < interval_length ? h_abs : interval_length;
        h_abs = h_abs < max_step ? h_abs : max_step;
    }

    T t = t0;
    const T inf = std::numeric_limits<T>::infinity();

    while (t != t1)
    {
        if (opts.max_steps != 0 && result.work.nsteps >= opts.max_steps)
        {
            result.status = OdeStatus::MaxSteps;
            result.t = t;
            return result;
        }

        // scipy _step_impl, verbatim.
        const T min_step = static_cast<T>(10) * std::abs(std::nextafter(t, direction * inf) - t);
        if (h_abs > max_step)
        {
            h_abs = max_step;
        }
        else if (h_abs < min_step)
        {
            h_abs = min_step;
        }

        bool step_accepted = false;
        T h = static_cast<T>(0);
        T t_new = t;

        while (!step_accepted)
        {
            if (h_abs < min_step)
            {
                result.status = OdeStatus::StepTooSmall;
                result.t = t;
                return result;
            }
            h = h_abs * direction;
            t_new = t + h;
            if (direction * (t_new - t1) > static_cast<T>(0))
            {
                t_new = t1;
            }
            h = t_new - t;
            h_abs = std::abs(h);

            // rk_step: stages 1..s−1.
            for (crd::usize st = 1; st < s; ++st)
            {
                for (crd::usize i = 0; i < n; ++i)
                {
                    T acc = static_cast<T>(0);
                    for (crd::usize j = 0; j < st; ++j)
                    {
                        acc += static_cast<T>(desc.a[st * s + j]) * kbuf[j * n + i];
                    }
                    ytmp[i] = y[i] + h * acc;
                }
                eval(t + static_cast<T>(desc.c[st]) * h, cont::ConstSpan<T>(ytmp.data(), n), krow(st));
            }
            // y_new = y + h·Bᵀ·K[:s]
            for (crd::usize i = 0; i < n; ++i)
            {
                T acc = static_cast<T>(0);
                for (crd::usize j = 0; j < s; ++j)
                {
                    acc += static_cast<T>(desc.b[j]) * kbuf[j * n + i];
                }
                y_new[i] = y[i] + h * acc;
            }
            // f_new — evaluated every attempt (scipy), stored as K[s].
            eval(t_new, cont::ConstSpan<T>(y_new.data(), n), krow(s));

            // Error norm.
            T error_norm;
            if (desc.e != nullptr)
            {
                T sum = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    T acc = static_cast<T>(0);
                    for (crd::usize j = 0; j <= s; ++j)
                    {
                        acc += static_cast<T>(desc.e[j]) * kbuf[j * n + i];
                    }
                    const T q = (acc * h) / scale_at(i, y[i], y_new[i]);
                    sum += q * q;
                }
                error_norm = crd::math::sqrt(sum / static_cast<T>(n));
            }
            else
            {
                // DOP853: |h|·‖err5‖² / √((‖err5‖² + 0.01·‖err3‖²)·n), errk = Kᵀ·Ek / scale.
                T e5sum = static_cast<T>(0);
                T e3sum = static_cast<T>(0);
                for (crd::usize i = 0; i < n; ++i)
                {
                    T a5 = static_cast<T>(0);
                    T a3 = static_cast<T>(0);
                    for (crd::usize j = 0; j <= s; ++j)
                    {
                        a5 += static_cast<T>(desc.e5[j]) * kbuf[j * n + i];
                        a3 += static_cast<T>(desc.e3[j]) * kbuf[j * n + i];
                    }
                    const T sc = scale_at(i, y[i], y_new[i]);
                    a5 /= sc;
                    a3 /= sc;
                    e5sum += a5 * a5;
                    e3sum += a3 * a3;
                }
                if (e5sum == static_cast<T>(0) && e3sum == static_cast<T>(0))
                {
                    error_norm = static_cast<T>(0);
                }
                else
                {
                    const T denom = e5sum + static_cast<T>(0.01) * e3sum;
                    error_norm = std::abs(h) * e5sum / crd::math::sqrt(denom * static_cast<T>(n));
                }
            }

            ++result.work.nsteps;
            bool accept = false;
            const T factor = controller.update(error_norm, accept);
            h_abs *= factor;
            if (accept)
            {
                step_accepted = true;
                ++result.work.naccept;
            }
            else
            {
                ++result.work.nreject;
            }
        }

        // Accepted: advance. FSAL bookkeeping = scipy's: f ← f_new (K[s] → K[0]).
        const T t_old = t;
        t = t_new;
        for (crd::usize i = 0; i < n; ++i)
        {
            ytmp[i] = y[i]; // y_old for event interpolation
            y[i] = y_new[i];
        }

        bool finite = true;
        for (crd::usize i = 0; i < n; ++i)
        {
            if (!std::isfinite(y[i]))
            {
                finite = false;
                break;
            }
        }
        if (!finite)
        {
            result.status = OdeStatus::NotFinite;
            result.t = t;
            return result;
        }

        // --- Events (v9-c, scipy semantics) over the step's Hermite interpolant ---
        if (!events.empty())
        {
            const cont::ConstSpan<T> y_old_c(ytmp.data(), n);
            const cont::ConstSpan<T> f_old_c = krow_c(0);
            const cont::ConstSpan<T> y_new_c(y.data(), n);
            const cont::ConstSpan<T> f_new_c = krow_c(s);

            T t_term = t; // earliest terminal root in step direction
            crd::i32 term_index = -1;
            cont::Array<T> y_at(alloc);
            y_at.resize(n);

            for (crd::usize ev = 0; ev < events.size(); ++ev)
            {
                const T g_new = events[ev]->value(t, y_new_c);
                const T g_old = g_prev[ev];
                const T dir = events[ev]->direction();
                const bool up = (g_old <= static_cast<T>(0)) && (g_new >= static_cast<T>(0));
                const bool down = (g_old >= static_cast<T>(0)) && (g_new <= static_cast<T>(0));
                bool fired = up || down;
                if (dir > static_cast<T>(0))
                {
                    fired = up;
                }
                else if (dir < static_cast<T>(0))
                {
                    fired = down;
                }
                if (fired && g_old != g_new)
                {
                    auto g_of_t = [&](T tt)
                    {
                        hermite_eval(t_old, t, y_old_c, f_old_c, y_new_c, f_new_c, tt, cont::Span<T>(y_at.data(), n));
                        return events[ev]->value(tt, cont::ConstSpan<T>(y_at.data(), n));
                    };
                    const T eps = std::numeric_limits<T>::epsilon();
                    const T root =
                        detail::brentq<T>(g_of_t, t_old, t, static_cast<T>(4) * eps, static_cast<T>(4) * eps);
                    if (events[ev]->hits() != nullptr)
                    {
                        events[ev]->hits()->push_back(root);
                    }
                    if (events[ev]->terminal() && direction * (root - t_term) <= static_cast<T>(0))
                    {
                        t_term = root;
                        term_index = static_cast<crd::i32>(ev);
                    }
                }
                g_prev[ev] = g_new;
            }

            if (term_index >= 0)
            {
                // Truncate to the event: y ← interp(t_term), final f for the solution node.
                hermite_eval(t_old, t, y_old_c, f_old_c, y_new_c, f_new_c, t_term, cont::Span<T>(y_at.data(), n));
                for (crd::usize i = 0; i < n; ++i)
                {
                    y[i] = y_at[i];
                }
                if (solution != nullptr)
                {
                    eval(t_term, cont::ConstSpan<T>(y.data(), n), krow(1)); // f at the event point
                    solution->append(t_term, cont::ConstSpan<T>(y.data(), n), krow_c(1));
                }
                result.status = OdeStatus::EventTerminal;
                result.t = t_term;
                result.event_index = term_index;
                return result;
            }
        }

        if (solution != nullptr)
        {
            solution->append(t, cont::ConstSpan<T>(y.data(), n), krow_c(s));
        }

        // FSAL: K[0] ← f_new.
        for (crd::usize i = 0; i < n; ++i)
        {
            kbuf[i] = kbuf[s * n + i];
        }
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
