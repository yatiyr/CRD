// bench_ode_imex_vs_arkode — v9-z IMEX work-precision crush vs the gold standard. The SAME 1D periodic
// advection-diffusion method-of-lines system (u_t = -c·u_x [explicit] + ν·u_xx [implicit]) is solved by
//   Cerid integrate_imex (ARK4 = ARK4(3)6L[2]SA)   vs   SUNDIALS ARKStep (its OWN ARK4(3)6L[2]SA table,
//   dense implicit linear solver + analytic implicit Jacobian) — apples-to-apples (identical method + split).
// Reports min-of-reps wall ms + the explicit/implicit RHS eval counts + the achieved max-error vs a tight
// Cerid DOP853 reference of the SAME discrete system, so rows are judged at ACHIEVED accuracy (the honest
// scoreboard rule). Compile (WSL, from repo root):
//   g++ -O2 -std=c++20 -I engine/hesap-ode/include -I engine/hesap/include -I engine/hesap-dense/include
//     -I engine/hesap-sparse/include -I engine/hesap-direct/include -I engine/hesap-iterative/include
//     -I engine/core/include -I build/linux-gcc-release/engine/core/include -I engine/containers/include
//     -I engine/memory/include -I engine/log/include -I engine/vm/include
//     runtime/examples/bench_ode_imex_vs_arkode.cpp
//     build/linux-gcc-release/engine/{hesap-dense,hesap,memory,vm,log}/libcrd-*.a
//     -lsundials_arkode -lsundials_core -lsundials_nvecserial -o /tmp/bench_imex
//   /tmp/bench_imex

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/imex.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <arkode/arkode_arkstep.h>
#include <nvector/nvector_serial.h>
#include <sundials/sundials_types.h>
#include <sunlinsol/sunlinsol_dense.h>
#include <sunmatrix/sunmatrix_dense.h>

#include <chrono>
#include <cmath>
#include <cstdio>

using crd::f64;
using crd::usize;
namespace ode = crd::hesap::ode;
namespace cont = crd::containers;

namespace
{
constexpr double kPi = 3.141592653589793;

struct Params
{
    int n;
    double c;
    double nu;
    double dx;
};

inline double adv(const double* u, int i, const Params& p)
{
    const int n = p.n;
    return -p.c * (u[(i + 1) % n] - u[(i + n - 1) % n]) / (2.0 * p.dx);
}
inline double dif(const double* u, int i, const Params& p)
{
    const int n = p.n;
    return p.nu * (u[(i + 1) % n] - 2.0 * u[i] + u[(i + n - 1) % n]) / (p.dx * p.dx);
}

// ---------------- Cerid IMEX form ----------------
class AdvDiff final : public ode::OdeFunction<f64>
{
public:
    explicit AdvDiff(Params p) : p_(p)
    {
        set_has_imex_split(true);
        set_has_implicit_jacobian(true);
    }
    void rhs(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override
    {
        for (int i = 0; i < p_.n; ++i)
            d[i] = adv(y.data(), i, p_) + dif(y.data(), i, p_);
    }
    [[nodiscard]] bool rhs_explicit(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override
    {
        for (int i = 0; i < p_.n; ++i)
            d[i] = adv(y.data(), i, p_);
        return true;
    }
    [[nodiscard]] bool rhs_implicit(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override
    {
        for (int i = 0; i < p_.n; ++i)
            d[i] = dif(y.data(), i, p_);
        return true;
    }
    [[nodiscard]] bool jacobian_implicit(f64, cont::ConstSpan<f64>, cont::Span<f64> j) const override
    {
        const int n = p_.n;
        for (int k = 0; k < n * n; ++k)
            j[k] = 0.0;
        const double co = p_.nu / (p_.dx * p_.dx);
        for (int i = 0; i < n; ++i)
        {
            j[i * n + i] = -2.0 * co;
            j[i * n + ((i + 1) % n)] += co;
            j[i * n + ((i + n - 1) % n)] += co;
        }
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return static_cast<usize>(p_.n); }

private:
    Params p_;
};

template <typename Fn> double time_best_ms(int reps, Fn&& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (ms < best)
            best = ms;
    }
    return best;
}

// ---------------- ARKODE form ----------------
int ark_fe(sunrealtype, N_Vector y, N_Vector ydot, void* ud)
{
    const Params& p = *static_cast<Params*>(ud);
    const double* u = N_VGetArrayPointer(y);
    double* d = N_VGetArrayPointer(ydot);
    for (int i = 0; i < p.n; ++i)
        d[i] = adv(u, i, p);
    return 0;
}
int ark_fi(sunrealtype, N_Vector y, N_Vector ydot, void* ud)
{
    const Params& p = *static_cast<Params*>(ud);
    const double* u = N_VGetArrayPointer(y);
    double* d = N_VGetArrayPointer(ydot);
    for (int i = 0; i < p.n; ++i)
        d[i] = dif(u, i, p);
    return 0;
}
int ark_jac(sunrealtype, N_Vector, N_Vector, SUNMatrix J, void* ud, N_Vector, N_Vector, N_Vector)
{
    const Params& p = *static_cast<Params*>(ud);
    const int n = p.n;
    SUNMatZero(J);
    const double co = p.nu / (p.dx * p.dx);
    for (int i = 0; i < n; ++i)
    {
        SM_ELEMENT_D(J, i, i) += -2.0 * co;
        SM_ELEMENT_D(J, i, (i + 1) % n) += co;
        SM_ELEMENT_D(J, i, (i + n - 1) % n) += co;
    }
    return 0;
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 26);
    const int n = 128;
    Params p{n, 1.0, 1.0, (2.0 * kPi) / static_cast<double>(n)};

    cont::Array<f64> u0(&alloc);
    u0.resize(n);
    for (int i = 0; i < n; ++i)
        u0[i] = std::sin(static_cast<double>(i) * p.dx);

    // Tight reference (DOP853 of the same discrete combined system).
    cont::Array<f64> uref(&alloc);
    uref.resize(n);
    for (int i = 0; i < n; ++i)
        uref[i] = u0[i];
    AdvDiff f(p);
    {
        ode::OdeOptions<f64> o;
        o.rtol = 1e-12;
        o.atol = 1e-14;
        const auto r = ode::integrate_erk<f64>(f, 0.0, 1.0, cont::Span<f64>(uref.data(), n), o, &alloc, ode::ErkMethod::Dop853);
        (void)r;
    }
    auto maxerr = [&](const cont::Array<f64>& u)
    {
        double m = 0.0;
        for (int i = 0; i < n; ++i)
            m = std::max(m, std::abs(u[i] - uref[i]));
        return m;
    };

    std::printf("=== IMEX advection-diffusion MOL, N=%d, c=%.1f nu=%.1f, t=1.0 (best of 20) ===\n", n, p.c, p.nu);

    // --- Run both solvers over a tolerance SWEEP and record the work-precision points. ---
    const double rtols[] = {1e-3, 1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9, 1e-10};
    constexpr int kN = 8;
    double c_err[kN], c_ms[kN], a_err[kN], a_ms[kN];
    long c_fev[kN], a_fev[kN];

    auto run_cerid = [&](double rtol, int idx)
    {
        cont::Array<f64> uc(&alloc);
        uc.resize(n);
        crd::u64 fev = 0;
        c_ms[idx] = time_best_ms(20,
                                 [&]
                                 {
                                     for (int i = 0; i < n; ++i)
                                         uc[i] = u0[i];
                                     ode::OdeOptions<f64> o;
                                     o.rtol = rtol;
                                     o.atol = rtol * 1e-3;
                                     const auto r = ode::integrate_imex<f64>(f, 0.0, 1.0, cont::Span<f64>(uc.data(), n),
                                                                             o, &alloc, ode::ImexMethod::Ark4);
                                     fev = r.work.nfev;
                                 });
        c_fev[idx] = static_cast<long>(fev);
        c_err[idx] = maxerr(uc);
    };
    auto run_arkode = [&](double rtol, int idx)
    {
        cont::Array<f64> ua(&alloc);
        ua.resize(n);
        long afe = 0, afi = 0;
        a_ms[idx] = time_best_ms(20,
                                 [&]
                                 {
                                     SUNContext ctx;
                                     SUNContext_Create(nullptr, &ctx);
                                     N_Vector y = N_VNew_Serial(n, ctx);
                                     double* yd = N_VGetArrayPointer(y);
                                     for (int i = 0; i < n; ++i)
                                         yd[i] = u0[i];
                                     void* ark = ARKStepCreate(ark_fe, ark_fi, 0.0, y, ctx);
                                     ARKStepSStolerances(ark, rtol, rtol * 1e-3);
                                     ARKStepSetUserData(ark, &p);
                                     ARKStepSetTableNum(ark, ARKODE_ARK436L2SA_DIRK_6_3_4, ARKODE_ARK436L2SA_ERK_6_3_4);
                                     SUNMatrix A = SUNDenseMatrix(n, n, ctx);
                                     SUNLinearSolver LS = SUNLinSol_Dense(y, A, ctx);
                                     ARKStepSetLinearSolver(ark, LS, A);
                                     ARKStepSetJacFn(ark, ark_jac);
                                     sunrealtype t = 0.0;
                                     ARKStepEvolve(ark, 1.0, y, &t, ARK_NORMAL);
                                     ARKStepGetNumRhsEvals(ark, &afe, &afi);
                                     double* yo = N_VGetArrayPointer(y);
                                     for (int i = 0; i < n; ++i)
                                         ua[i] = yo[i];
                                     ARKStepFree(&ark);
                                     SUNLinSolFree(LS);
                                     SUNMatDestroy(A);
                                     N_VDestroy(y);
                                     SUNContext_Free(&ctx);
                                 });
        a_fev[idx] = afe + afi;
        a_err[idx] = maxerr(ua);
    };

    std::printf("\n-- work-precision sweep (achieved error is the honest axis) --\n");
    std::printf("%-7s | %-30s | %-30s\n", "rtol", "Cerid IMEX ARK4 (err / ms / fev)", "ARKODE ARK4 (err / ms / fev)");
    for (int i = 0; i < kN; ++i)
    {
        run_cerid(rtols[i], i);
        run_arkode(rtols[i], i);
        std::printf("%-7.0e | %.2e / %6.3f ms / %5ld | %.2e / %6.3f ms / %5ld\n", rtols[i], c_err[i], c_ms[i],
                    c_fev[i], a_err[i], a_ms[i], a_fev[i]);
    }

    // --- MATCHED-ACCURACY readout: for each target error, the work-optimal (coarsest-rtol) point that
    //     achieves it, per solver — the apples-to-apples comparison at EQUAL tightness. ---
    std::printf("\n-- matched ACHIEVED accuracy (work-optimal point reaching the target err) --\n");
    std::printf("%-10s | %-26s | %-26s | crush\n", "target err", "Cerid (ms / fev)", "ARKODE (ms / fev)");
    for (double target : {1e-5, 1e-7, 1e-9})
    {
        int ci = -1, ai = -1;
        for (int i = 0; i < kN; ++i)
            if (c_err[i] <= target && (ci < 0 || c_ms[i] < c_ms[ci]))
                ci = i;
        for (int i = 0; i < kN; ++i)
            if (a_err[i] <= target && (ai < 0 || a_ms[i] < a_ms[ai]))
                ai = i;
        if (ci < 0 || ai < 0)
        {
            std::printf("%-10.0e | (not reached in sweep)\n", target);
            continue;
        }
        std::printf("%-10.0e | %6.3f ms / %5ld          | %6.3f ms / %5ld          | %.2fx wall, %.2fx fewer evals\n",
                    target, c_ms[ci], c_fev[ci], a_ms[ai], a_fev[ai], a_ms[ai] / c_ms[ci],
                    static_cast<double>(a_fev[ai]) / static_cast<double>(c_fev[ci]));
    }
    return 0;
}
