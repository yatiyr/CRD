#pragma once

// bdf.hpp — Phase 3.1.6 v9-d: variable-order (1–5) BDF/NDF with scipy `solve_ivp` EXACT semantics
// (scipy 1.17.1 `_ivp/bdf.py` read verbatim — the v9-b playbook applied to the stiff spine):
//   • the Shampine-Reichelt NDF formulation (κ = [0, −0.1850, −1/9, −0.0823, −0.0415, 0]) over the
//     backward-difference array D (rows D⁰..D^{order+2}),
//   • `compute_R`/`change_D` step rescaling, `solve_bdf_system` Newton (max 4 iterations, the rate-based
//     divergence and convergence predicates verbatim), newton_tol = max(10·eps/rtol, min(0.03, √rtol)),
//   • the step loop verbatim INCLUDING the documented quirk that an error-rejected step keeps the stale
//     LU (scipy: "we don't reset LU here") — exactness demands replicating it,
//   • order selection after order+1 equal steps via the (order−1, order, order+1) error-norm factors,
//   • Jacobian policy: analytic when `OdeFunction::has_jacobian()` (the trajectory-exact configuration —
//     scipy's `jac=callable`); otherwise plain forward differences (NAMED divergence: scipy's adaptive
//     `num_jac` is not replicated — FD trajectories are correct but not bit-matched).
// Linear algebra through the v9-d `OdeLinearSolver` seam (dense hesap-dense LU now; sparse/Krylov at
// v9-j). ADR-0091.

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

inline constexpr crd::usize bdf_max_order = 5;
inline constexpr crd::u32 bdf_newton_maxiter = 4;

// NDF constants (scipy bdf.py): gamma_k = sum_{i=1..k} 1/i; alpha = (1-kappa)*gamma;
// error_const_k = kappa_k*gamma_k + 1/(k+1).
inline constexpr crd::f64 bdf_kappa[6] = {0.0, -0.1850, -1.0 / 9.0, -0.0823, -0.0415, 0.0};
inline constexpr crd::f64 bdf_gamma[6] = {0.0,
                                          1.0,
                                          1.0 + 1.0 / 2.0,
                                          1.0 + 1.0 / 2.0 + 1.0 / 3.0,
                                          1.0 + 1.0 / 2.0 + 1.0 / 3.0 + 1.0 / 4.0,
                                          1.0 + 1.0 / 2.0 + 1.0 / 3.0 + 1.0 / 4.0 + 1.0 / 5.0};

[[nodiscard]] constexpr crd::f64 bdf_alpha(crd::usize k) noexcept
{
    return (1.0 - bdf_kappa[k]) * bdf_gamma[k];
}
[[nodiscard]] constexpr crd::f64 bdf_error_const(crd::usize k) noexcept
{
    return bdf_kappa[k] * bdf_gamma[k] + 1.0 / static_cast<crd::f64>(k + 1);
}

// compute_R(order, factor) into r (row-major (order+1)x(order+1)): M[0][:] = 1; M[i][j] = (i-1-factor*j)/i
// for i,j >= 1 (M[i][0] = 0); R = column-wise cumulative product down the rows.
template <typename T> void bdf_compute_r(crd::usize order, T factor, T* r)
{
    const crd::usize m = order + 1;
    for (crd::usize j = 0; j < m; ++j)
    {
        r[j] = static_cast<T>(1);
    }
    for (crd::usize i = 1; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            const T mij =
                (j == 0) ? static_cast<T>(0)
                         : (static_cast<T>(i) - static_cast<T>(1) - factor * static_cast<T>(j)) / static_cast<T>(i);
            r[i * m + j] = r[(i - 1) * m + j] * mij;
        }
    }
}

// change_D: D[:order+1] = (R·U)ᵀ · D[:order+1], with U = compute_R(order, 1). D row stride = n.
template <typename T> void bdf_change_d(T* d, crd::usize n, crd::usize order, T factor, T* work)
{
    const crd::usize m = order + 1;
    T r[(bdf_max_order + 1) * (bdf_max_order + 1)];
    T u[(bdf_max_order + 1) * (bdf_max_order + 1)];
    T ru[(bdf_max_order + 1) * (bdf_max_order + 1)];
    bdf_compute_r(order, factor, r);
    bdf_compute_r(order, static_cast<T>(1), u);
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize j = 0; j < m; ++j)
        {
            T acc = static_cast<T>(0);
            for (crd::usize k = 0; k < m; ++k)
            {
                acc += r[i * m + k] * u[k * m + j];
            }
            ru[i * m + j] = acc;
        }
    }
    // work = RUᵀ·D (m rows × n)
    for (crd::usize i = 0; i < m; ++i)
    {
        for (crd::usize col = 0; col < n; ++col)
        {
            T acc = static_cast<T>(0);
            for (crd::usize k = 0; k < m; ++k)
            {
                acc += ru[k * m + i] * d[k * n + col];
            }
            work[i * n + col] = acc;
        }
    }
    for (crd::usize i = 0; i < m * n; ++i)
    {
        d[i] = work[i];
    }
}

} // namespace detail

// Variable-order BDF/NDF (the stiff driver). `solver` may be user-supplied (v9-j: sparse/Krylov); nullptr
// ⇒ an internal dense hesap-dense LU. `solution` recording costs ONE extra rhs eval per accepted step
// (BDF's Newton does not naturally leave f(t_new, y_new) behind) — counted, and disabled in the
// trajectory-exactness difftest.
template <typename T>
[[nodiscard]] OdeResult<T> integrate_bdf(const OdeFunction<T>& fn, T t0, T t1, crd::containers::Span<T> y,
                                         const OdeOptions<T>& opts, crd::memory::IAllocator* alloc,
                                         OdeLinearSolver<T>* solver = nullptr, OdeSolution<T>* solution = nullptr)
{
    namespace cont = crd::containers;
    using detail::bdf_max_order;
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

    // Sparse-Jacobian functions REQUIRE an explicit sparse-capable solver (SparseOdeLinearSolver) —
    // bdf.hpp deliberately carries no hesap-direct edge; the dense default covers everything else.
    CRD_ASSERT(!fn.has_sparse_jacobian() || solver != nullptr);
    DenseOdeLinearSolver<T> internal_solver(alloc);
    OdeLinearSolver<T>* lin = (solver != nullptr) ? solver : &internal_solver;

    // v9-j (Krylov): a matrix-free solver (KrylovOdeLinearSolver) is driven WITHOUT any Jacobian assembly —
    // the linearization point is recorded and the inner solve is GMRES over jacobian_vector (CVODE SPGMR).
    const bool use_matfree = (solver != nullptr) && solver->is_matrix_free();
    CRD_ASSERT(!use_matfree || fn.has_jacobian_vector());

    const T direction = (t1 > t0) ? static_cast<T>(1) : static_cast<T>(-1);
    const T interval_length = std::abs(t1 - t0);
    const T max_step = opts.hmax;
    const T eps = std::numeric_limits<T>::epsilon();
    const T newton_tol_lo = static_cast<T>(10) * eps / opts.rtol;
    const T newton_tol_hi = crd::math::sqrt(opts.rtol) < static_cast<T>(0.03) ? crd::math::sqrt(opts.rtol) : static_cast<T>(0.03);
    const T newton_tol = newton_tol_lo > newton_tol_hi ? newton_tol_lo : newton_tol_hi;

    auto atol_i = [&opts](crd::usize i)
    {
        return opts.atol_vec.empty() ? opts.atol : opts.atol_vec[i];
    };
    auto rms = [n](cont::ConstSpan<T> v)
    {
        T sum = static_cast<T>(0);
        for (crd::usize i = 0; i < v.size(); ++i)
        {
            sum += v[i] * v[i];
        }
        return crd::math::sqrt(sum / static_cast<T>(n));
    };

    // Workspace.
    cont::Array<T> dbuf(alloc); // D: (max_order + 3) rows x n
    dbuf.resize((bdf_max_order + 3) * n);
    cont::Array<T> work(alloc); // change_D scratch ((max_order + 1) x n)
    work.resize((bdf_max_order + 1) * n);
    cont::Array<T> jac(alloc); // dense row-major Jacobian (NOT allocated on the sparse/matrix-free path)
    jac.resize((fn.has_sparse_jacobian() || use_matfree) ? 0 : n * n);
    // v9-j matrix-free: the linearization point (no dense J ever formed).
    cont::Array<T> ylin(alloc);
    if (use_matfree)
    {
        ylin.resize(n);
    }
    T tlin = t0;
    cont::Array<T> y_predict(alloc);
    y_predict.resize(n);
    cont::Array<T> psi(alloc);
    psi.resize(n);
    cont::Array<T> y_new(alloc);
    y_new.resize(n);
    cont::Array<T> dsum(alloc);
    dsum.resize(n);
    cont::Array<T> fvec(alloc);
    fvec.resize(n);
    cont::Array<T> dy(alloc);
    dy.resize(n);
    cont::Array<T> scale(alloc);
    scale.resize(n);
    cont::Array<T> ytmp(alloc);
    ytmp.resize(n);
    T* D = dbuf.data();

    // v9-j: SPARSE Jacobian path (large-n MOL — takes precedence over the dense capability; requires a
    // sparse-capable solver, e.g. SparseOdeLinearSolver). Mass × sparse = a named follow-on combination.
    const bool use_sparse = fn.has_sparse_jacobian();
    crd::hesap::sparse::SparseMatrix<T, crd::hesap::sparse::SparseFormat::Csr> sjac(alloc);

    // v9-h: constant (possibly singular) mass matrix — M·y' = f. The M-less path below is BYTE-IDENTICAL
    // to the scipy-exact v9-d code (every mass branch is behind `has_mass`).
    const bool has_mass = fn.has_mass_matrix();
    CRD_ASSERT(!(has_mass && use_sparse));                    // named follow-on, not yet wired
    CRD_ASSERT(!(use_matfree && (use_sparse || has_mass)));   // matrix-free × sparse/mass = named follow-on
    cont::Array<T> mass(alloc);
    cont::Array<T> mv(alloc);
    if (has_mass)
    {
        mass.resize(n * n);
        mv.resize(n);
        const bool ok = fn.mass_matrix(cont::Span<T>(mass.data(), n * n));
        CRD_ASSERT(ok);
        (void)ok;
    }

    auto eval = [&fn, &result](T t, cont::ConstSpan<T> yy, cont::Span<T> out)
    {
        fn.rhs(t, yy, out);
        ++result.work.nfev;
    };

    // Jacobian: sparse if provided (v9-j), else analytic dense, else plain forward differences (NAMED
    // non-scipy-exact fallback).
    auto build_jacobian = [&](T t, cont::ConstSpan<T> yy)
    {
        if (use_matfree)
        {
            // No dense J: just record the linearization point for the matrix-free operator.
            tlin = t;
            for (crd::usize i = 0; i < n; ++i)
            {
                ylin[i] = yy[i];
            }
            ++result.work.njev;
            return;
        }
        if (use_sparse)
        {
            const bool ok = fn.sparse_jacobian(t, yy, sjac);
            CRD_ASSERT(ok);
            (void)ok;
            ++result.work.njev;
            return;
        }
        if (fn.has_jacobian())
        {
            const bool ok = fn.jacobian(t, yy, cont::Span<T>(jac.data(), n * n));
            CRD_ASSERT(ok);
            (void)ok;
            ++result.work.njev;
            return;
        }
        eval(t, yy, cont::Span<T>(fvec.data(), n));
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
            eval(t, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(dy.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                jac[i * n + j] = (dy[i] - fvec[i]) / hj;
            }
            ytmp[j] = yj;
        }
        ++result.work.njev;
    };

    // f0 + initial step (scipy BDF: select_initial_step with order = 1).
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
        const T d0 = crd::math::sqrt(d0s / static_cast<T>(n));
        const T d1 = crd::math::sqrt(d1s / static_cast<T>(n));
        T h0_try = (d0 < static_cast<T>(1e-5) || d1 < static_cast<T>(1e-5)) ? static_cast<T>(1e-6)
                                                                            : static_cast<T>(0.01) * d0 / d1;
        h0_try = h0_try < interval_length ? h0_try : interval_length;
        for (crd::usize i = 0; i < n; ++i)
        {
            ytmp[i] = y[i] + h0_try * direction * fvec[i];
        }
        eval(t0 + h0_try * direction, cont::ConstSpan<T>(ytmp.data(), n), cont::Span<T>(dy.data(), n));
        T d2s = static_cast<T>(0);
        for (crd::usize i = 0; i < n; ++i)
        {
            const T sc = atol_i(i) + std::abs(y[i]) * opts.rtol;
            const T q = (dy[i] - fvec[i]) / sc;
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
            h1 = crd::math::pow(static_cast<T>(0.01) / dm, static_cast<T>(1) / static_cast<T>(2)); // order 1 ⇒ 1/(1+1)
        }
        h_abs = static_cast<T>(100) * h0_try;
        h_abs = h_abs < h1 ? h_abs : h1;
        h_abs = h_abs < interval_length ? h_abs : interval_length;
        h_abs = h_abs < max_step ? h_abs : max_step;
    }

    // D init, J init, state.
    build_jacobian(t0, cont::ConstSpan<T>(y.data(), n));
    for (crd::usize i = 0; i < n; ++i)
    {
        D[i] = y[i];
    }
    if (!has_mass)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            D[n + i] = fvec[i] * h_abs * direction;
        }
    }
    else
    {
        // D[1] = h·ẏ₀: solve M·v = f₀ when M is regular; for SINGULAR M (index-1 DAE) use D[1] = 0
        // (predictor = y₀ — the first implicit-Euler Newton supplies the slope; y₀ must be CONSISTENT,
        // the caller's contract; `calc_ic` consistent initialization = the named follow-up).
        for (crd::usize i = 0; i < n; ++i)
        {
            mv[i] = fvec[i];
        }
        // c = 0 ⇒ the iteration matrix is M itself (the jac content is irrelevant at c = 0).
        if (lin->factor_iteration_matrix_mass(static_cast<T>(0), cont::ConstSpan<T>(jac.data(), n * n),
                                              cont::ConstSpan<T>(mass.data(), n * n), n))
        {
            lin->solve(cont::Span<T>(mv.data(), n));
            for (crd::usize i = 0; i < n; ++i)
            {
                D[n + i] = mv[i] * h_abs * direction;
            }
        }
        else
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                D[n + i] = static_cast<T>(0);
            }
        }
    }

    crd::usize order = 1;
    crd::usize n_equal_steps = 0;
    bool lu_valid = false;
    bool current_jac = false; // analytic/FD jac is refreshable ⇒ scipy's `self.jac is None` = false
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
        if (h_abs > max_step)
        {
            detail::bdf_change_d(D, n, order, max_step / h_abs, work.data());
            h_abs = max_step;
            n_equal_steps = 0;
        }
        else if (h_abs < min_step)
        {
            detail::bdf_change_d(D, n, order, min_step / h_abs, work.data());
            h_abs = min_step;
            n_equal_steps = 0;
        }

        bool step_accepted = false;
        T t_new = t;
        T safety = static_cast<T>(0);
        T error_norm = static_cast<T>(0);
        current_jac = false; // per scipy: re-derived each _step_impl entry (callable jac ⇒ false)

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
                detail::bdf_change_d(D, n, order, std::abs(t_new - t) / h_abs, work.data());
                n_equal_steps = 0;
                lu_valid = false;
            }
            h = t_new - t;
            h_abs = std::abs(h);

            // y_predict = sum D[0..order]; psi = (D[1..order]^T · gamma[1..order]) / alpha[order].
            for (crd::usize i = 0; i < n; ++i)
            {
                T acc = static_cast<T>(0);
                for (crd::usize k = 0; k <= order; ++k)
                {
                    acc += D[k * n + i];
                }
                y_predict[i] = acc;
                T p = static_cast<T>(0);
                for (crd::usize k = 1; k <= order; ++k)
                {
                    p += D[k * n + i] * static_cast<T>(detail::bdf_gamma[k]);
                }
                psi[i] = p / static_cast<T>(detail::bdf_alpha(order));
                scale[i] = atol_i(i) + opts.rtol * std::abs(y_predict[i]);
            }

            const T c = h / static_cast<T>(detail::bdf_alpha(order));

            bool converged = false;
            crd::u32 n_iter = 0;
            while (!converged)
            {
                // scipy-exact: refactor ONLY when no factorization exists — a stale-c LU after a
                // rejection is deliberately kept (Newton still converges against it).
                if (!lu_valid)
                {
                    bool ok;
                    if (use_matfree)
                    {
                        ok = lin->factor_iteration_matrix_matfree(fn, tlin, cont::ConstSpan<T>(ylin.data(), n), c);
                    }
                    else if (use_sparse)
                    {
                        ok = lin->factor_iteration_matrix_sparse(c, sjac);
                    }
                    else if (has_mass)
                    {
                        ok = lin->factor_iteration_matrix_mass(c, cont::ConstSpan<T>(jac.data(), n * n),
                                                               cont::ConstSpan<T>(mass.data(), n * n), n);
                    }
                    else
                    {
                        ok = lin->factor_iteration_matrix(c, cont::ConstSpan<T>(jac.data(), n * n), n);
                    }
                    ++result.work.nlu;
                    lu_valid = ok;
                    if (!ok)
                    {
                        break; // singular iteration matrix ⇒ treated as convergence failure below
                    }
                }

                // solve_bdf_system (scipy, verbatim).
                for (crd::usize i = 0; i < n; ++i)
                {
                    dsum[i] = static_cast<T>(0);
                    y_new[i] = y_predict[i];
                }
                T dy_norm_old = static_cast<T>(-1);
                converged = false;
                for (crd::u32 k = 0; k < detail::bdf_newton_maxiter; ++k)
                {
                    n_iter = k + 1;
                    eval(t_new, cont::ConstSpan<T>(y_new.data(), n), cont::Span<T>(fvec.data(), n));
                    bool finite = true;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        if (!std::isfinite(fvec[i]))
                        {
                            finite = false;
                            break;
                        }
                    }
                    if (!finite)
                    {
                        break;
                    }
                    if (!has_mass)
                    {
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            dy[i] = c * fvec[i] - psi[i] - dsum[i];
                        }
                    }
                    else
                    {
                        // residual: c·f − M·(psi + d)
                        for (crd::usize i = 0; i < n; ++i)
                        {
                            T acc = static_cast<T>(0);
                            for (crd::usize j = 0; j < n; ++j)
                            {
                                acc += mass[i * n + j] * (psi[j] + dsum[j]);
                            }
                            dy[i] = c * fvec[i] - acc;
                        }
                    }
                    lin->solve(cont::Span<T>(dy.data(), n));
                    ++result.work.nsol;
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        ytmp[i] = dy[i] / scale[i];
                    }
                    const T dy_norm = rms(cont::ConstSpan<T>(ytmp.data(), n));
                    const bool have_rate = dy_norm_old >= static_cast<T>(0);
                    const T rate = have_rate ? dy_norm / dy_norm_old : static_cast<T>(0);
                    if (have_rate &&
                        (rate >= static_cast<T>(1) || crd::math::pow(rate, static_cast<T>(detail::bdf_newton_maxiter - k)) /
                                                              (static_cast<T>(1) - rate) * dy_norm >
                                                          newton_tol))
                    {
                        break;
                    }
                    for (crd::usize i = 0; i < n; ++i)
                    {
                        y_new[i] += dy[i];
                        dsum[i] += dy[i];
                    }
                    if (dy_norm == static_cast<T>(0) ||
                        (have_rate && rate / (static_cast<T>(1) - rate) * dy_norm < newton_tol))
                    {
                        converged = true;
                        break;
                    }
                    dy_norm_old = dy_norm;
                }

                if (!converged)
                {
                    if (current_jac)
                    {
                        break;
                    }
                    build_jacobian(t_new, cont::ConstSpan<T>(y_predict.data(), n));
                    lu_valid = false;
                    current_jac = true;
                }
            }

            ++result.work.nsteps;
            if (!converged)
            {
                const T factor = static_cast<T>(0.5);
                h_abs *= factor;
                detail::bdf_change_d(D, n, order, factor, work.data());
                n_equal_steps = 0;
                lu_valid = false;
                ++result.work.nreject;
                continue;
            }

            safety = static_cast<T>(0.9) * static_cast<T>(2 * detail::bdf_newton_maxiter + 1) /
                     static_cast<T>(2 * detail::bdf_newton_maxiter + n_iter);

            for (crd::usize i = 0; i < n; ++i)
            {
                scale[i] = atol_i(i) + opts.rtol * std::abs(y_new[i]);
                ytmp[i] = static_cast<T>(detail::bdf_error_const(order)) * dsum[i] / scale[i];
            }
            error_norm = rms(cont::ConstSpan<T>(ytmp.data(), n));

            if (error_norm > static_cast<T>(1))
            {
                T factor = safety * crd::math::pow(error_norm, static_cast<T>(-1) / static_cast<T>(order + 1));
                if (factor < static_cast<T>(0.2))
                {
                    factor = static_cast<T>(0.2);
                }
                h_abs *= factor;
                detail::bdf_change_d(D, n, order, factor, work.data());
                n_equal_steps = 0;
                ++result.work.nreject;
                // scipy: the LU is deliberately NOT reset here (stale-c Newton still converges).
            }
            else
            {
                step_accepted = true;
                ++result.work.naccept;
            }
        }

        n_equal_steps += 1;
        t = t_new;
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = y_new[i];
        }

        // D update: D[order+2] = d − D[order+1]; D[order+1] = d; D[i] += D[i+1] downward.
        for (crd::usize i = 0; i < n; ++i)
        {
            D[(order + 2) * n + i] = dsum[i] - D[(order + 1) * n + i];
            D[(order + 1) * n + i] = dsum[i];
        }
        for (crd::usize k = order + 1; k-- > 0;)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                D[k * n + i] += D[(k + 1) * n + i];
            }
        }

        if (solution != nullptr)
        {
            eval(t, cont::ConstSpan<T>(y.data(), n), cont::Span<T>(fvec.data(), n));
            solution->append(t, cont::ConstSpan<T>(y.data(), n), cont::ConstSpan<T>(fvec.data(), n));
        }

        if (n_equal_steps < order + 1)
        {
            continue;
        }

        // Order selection (scipy, verbatim; scale is at y_new from the accepted step).
        T error_m_norm = std::numeric_limits<T>::infinity();
        if (order > 1)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                ytmp[i] = static_cast<T>(detail::bdf_error_const(order - 1)) * D[order * n + i] / scale[i];
            }
            error_m_norm = rms(cont::ConstSpan<T>(ytmp.data(), n));
        }
        T error_p_norm = std::numeric_limits<T>::infinity();
        if (order < bdf_max_order)
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                ytmp[i] = static_cast<T>(detail::bdf_error_const(order + 1)) * D[(order + 2) * n + i] / scale[i];
            }
            error_p_norm = rms(cont::ConstSpan<T>(ytmp.data(), n));
        }

        const T norms[3] = {error_m_norm, error_norm, error_p_norm};
        T factors[3];
        for (int idx = 0; idx < 3; ++idx)
        {
            const T expo = static_cast<T>(-1) / static_cast<T>(order + static_cast<crd::usize>(idx));
            factors[idx] =
                (norms[idx] == static_cast<T>(0)) ? std::numeric_limits<T>::infinity() : crd::math::pow(norms[idx], expo);
        }
        int best = 0;
        for (int idx = 1; idx < 3; ++idx)
        {
            if (factors[idx] > factors[best])
            {
                best = idx; // strict > == np.argmax first-max tie-breaking
            }
        }
        order = order + static_cast<crd::usize>(best) - 1;

        T factor = safety * factors[best];
        if (factor > static_cast<T>(10))
        {
            factor = static_cast<T>(10);
        }
        h_abs *= factor;
        detail::bdf_change_d(D, n, order, factor, work.data());
        n_equal_steps = 0;
        lu_valid = false;
    }

    result.status = OdeStatus::Success;
    result.success = true;
    result.t = t1;
    return result;
}

} // namespace crd::hesap::ode
