// bench_ode_sens_vs_cvodes — v9-z work-precision row: forward parameter sensitivities vs the gold standard.
// Robertson-with-parameters (n=3, np=3 rate constants) — the canonical CVODES FSA example. Compares
//   Cerid integrate_forward_sensitivities (CVODES simultaneous corrector, augmented [y;S], analytic J_y +
//   df/dp)   vs   SUNDIALS CVODES forward sensitivity analysis (CV_SIMULTANEOUS, DQ sensitivity RHS — the
//   standard CVODES usage).
// THE primary check is VALUE AGREEMENT: both compute dy/dp(T); they must match. Plus wall + RHS evals.
// (Cerid uses ANALYTIC sensitivity RHS; CVODES here uses its difference-quotient default — named; so the
// eval counts are not apples-to-apples, the value agreement is the cross-validation and wall is the perf.)
// Compile (WSL): scripts/run_bench_sens.sh.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/sensitivity.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cvodes/cvodes.h>
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
double g_p[3] = {0.04, 1e4, 3e7};

// ---------------- Cerid form ----------------
class RobertsonP final : public ode::ParametricOdeFunction<f64>
{
public:
    void rhs(f64, cont::ConstSpan<f64> y, cont::ConstSpan<f64> p, cont::Span<f64> d) const override
    {
        d[0] = -p[0] * y[0] + p[1] * y[1] * y[2];
        d[1] = p[0] * y[0] - p[1] * y[1] * y[2] - p[2] * y[1] * y[1];
        d[2] = p[2] * y[1] * y[1];
    }
    void jacobian_y(f64, cont::ConstSpan<f64> y, cont::ConstSpan<f64> p, cont::Span<f64> j) const override
    {
        j[0] = -p[0];
        j[1] = p[1] * y[2];
        j[2] = p[1] * y[1];
        j[3] = p[0];
        j[4] = -p[1] * y[2] - 2.0 * p[2] * y[1];
        j[5] = -p[1] * y[1];
        j[6] = 0.0;
        j[7] = 2.0 * p[2] * y[1];
        j[8] = 0.0;
    }
    void dfdp(f64, cont::ConstSpan<f64> y, cont::ConstSpan<f64>, usize jp, cont::Span<f64> out) const override
    {
        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 0.0;
        if (jp == 0)
        {
            out[0] = -y[0];
            out[1] = y[0];
        }
        else if (jp == 1)
        {
            out[0] = y[1] * y[2];
            out[1] = -y[1] * y[2];
        }
        else
        {
            out[1] = -y[1] * y[1];
            out[2] = y[1] * y[1];
        }
    }
    [[nodiscard]] usize dim() const noexcept override { return 3; }
    [[nodiscard]] usize n_params() const noexcept override { return 3; }
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
        best = ms < best ? ms : best;
    }
    return best;
}

// ---------------- CVODES form ----------------
int cv_rhs(sunrealtype, N_Vector y, N_Vector ydot, void* ud)
{
    const double* p = static_cast<double*>(ud);
    const double* u = N_VGetArrayPointer(y);
    double* d = N_VGetArrayPointer(ydot);
    d[0] = -p[0] * u[0] + p[1] * u[1] * u[2];
    d[1] = p[0] * u[0] - p[1] * u[1] * u[2] - p[2] * u[1] * u[1];
    d[2] = p[2] * u[1] * u[1];
    return 0;
}
int cv_jac(sunrealtype, N_Vector y, N_Vector, SUNMatrix J, void* ud, N_Vector, N_Vector, N_Vector)
{
    const double* p = static_cast<double*>(ud);
    const double* u = N_VGetArrayPointer(y);
    SM_ELEMENT_D(J, 0, 0) = -p[0];
    SM_ELEMENT_D(J, 0, 1) = p[1] * u[2];
    SM_ELEMENT_D(J, 0, 2) = p[1] * u[1];
    SM_ELEMENT_D(J, 1, 0) = p[0];
    SM_ELEMENT_D(J, 1, 1) = -p[1] * u[2] - 2.0 * p[2] * u[1];
    SM_ELEMENT_D(J, 1, 2) = -p[1] * u[1];
    SM_ELEMENT_D(J, 2, 0) = 0.0;
    SM_ELEMENT_D(J, 2, 1) = 2.0 * p[2] * u[1];
    SM_ELEMENT_D(J, 2, 2) = 0.0;
    return 0;
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 24);
    const usize n = 3, np = 3;
    const double y0[3] = {1.0, 0.0, 0.0};
    const double t1 = 1e4;
    const double rtol = 1e-8, atol = 1e-12;

    // ---- Cerid forward sensitivities (stiff = BDF augmented) ----
    cont::Array<f64> y_c(&alloc), s_c(&alloc);
    double cms = time_best_ms(10,
                              [&]
                              {
                                  y_c.resize(n);
                                  for (usize i = 0; i < n; ++i)
                                      y_c[i] = y0[i];
                                  s_c.resize(n * np);
                                  for (usize i = 0; i < n * np; ++i)
                                      s_c[i] = 0.0;
                                  ode::OdeOptions<f64> o;
                                  o.rtol = rtol;
                                  o.atol = atol;
                                  RobertsonP pfn;
                                  (void)ode::integrate_forward_sensitivities<f64>(
                                      pfn, 0.0, t1, cont::Span<f64>(y_c.data(), n), cont::Span<f64>(s_c.data(), n * np),
                                      cont::ConstSpan<f64>(g_p, np), o, &alloc, /*stiff*/ true);
                              });

    // ---- CVODES forward sensitivity analysis (DQ sensitivity RHS) ----
    double s_cv[9] = {0};
    long cv_nfev = 0, cv_nfes = 0;
    double ams = time_best_ms(10,
                              [&]
                              {
                                  SUNContext ctx;
                                  SUNContext_Create(nullptr, &ctx);
                                  N_Vector y = N_VNew_Serial(static_cast<sunindextype>(n), ctx);
                                  double* yd = N_VGetArrayPointer(y);
                                  for (usize i = 0; i < n; ++i)
                                      yd[i] = y0[i];
                                  void* mem = CVodeCreate(CV_BDF, ctx);
                                  CVodeInit(mem, cv_rhs, 0.0, y);
                                  CVodeSStolerances(mem, rtol, atol);
                                  CVodeSetMaxNumSteps(mem, 1000000); // default 500 is far too few for t=1e4
                                  CVodeSetUserData(mem, g_p);
                                  SUNMatrix A = SUNDenseMatrix(static_cast<sunindextype>(n), static_cast<sunindextype>(n), ctx);
                                  SUNLinearSolver LS = SUNLinSol_Dense(y, A, ctx);
                                  CVodeSetLinearSolver(mem, LS, A);
                                  CVodeSetJacFn(mem, cv_jac);
                                  N_Vector* yS = N_VCloneVectorArray(static_cast<int>(np), y);
                                  for (usize j = 0; j < np; ++j)
                                      N_VConst(0.0, yS[j]);
                                  CVodeSensInit(mem, static_cast<int>(np), CV_SIMULTANEOUS, nullptr, yS); // DQ rhs
                                  CVodeSensEEtolerances(mem);
                                  double pbar[3] = {g_p[0], g_p[1], g_p[2]};
                                  CVodeSetSensParams(mem, g_p, pbar, nullptr);
                                  sunrealtype t = 0.0;
                                  CVode(mem, t1, y, &t, CV_NORMAL);
                                  CVodeGetSens(mem, &t, yS);
                                  for (usize j = 0; j < np; ++j)
                                  {
                                      const double* sj = N_VGetArrayPointer(yS[j]);
                                      for (usize i = 0; i < n; ++i)
                                          s_cv[j * n + i] = sj[i];
                                  }
                                  CVodeGetNumRhsEvals(mem, &cv_nfev);
                                  CVodeGetSensNumRhsEvals(mem, &cv_nfes);
                                  N_VDestroyVectorArray(yS, static_cast<int>(np));
                                  CVodeFree(&mem);
                                  SUNLinSolFree(LS);
                                  SUNMatDestroy(A);
                                  N_VDestroy(y);
                                  SUNContext_Free(&ctx);
                              });

    // ---- compare ----
    std::printf("=== forward sensitivities: Robertson-with-params (n=3, np=3), t=%.0e, rtol=%.0e ===\n", t1, rtol);
    std::printf("\nS = dy/dp(T)   [Cerid analytic FSA   vs   CVODES DQ FSA]\n");
    double maxrel = 0.0;
    for (usize j = 0; j < np; ++j)
        for (usize i = 0; i < n; ++i)
        {
            const double a = s_c[j * n + i], b = s_cv[j * n + i];
            const double rel = std::abs(a - b) / (1.0 + std::abs(b));
            maxrel = std::max(maxrel, rel);
            std::printf("  dy[%zu]/dp[%zu] : % .6e  vs  % .6e\n", i, j, a, b);
        }
    std::printf("\nmax relative |Cerid - CVODES| = %.2e  (value cross-validation)\n", maxrel);
    std::printf("wall: Cerid %.3f ms  vs  CVODES %.3f ms  [%.2fx]\n", cms, ams, ams / cms);
    std::printf("CVODES RHS evals: state %ld + sensitivity %ld (DQ); Cerid uses analytic J_y + df/dp.\n", cv_nfev,
                cv_nfes);
    return 0;
}
