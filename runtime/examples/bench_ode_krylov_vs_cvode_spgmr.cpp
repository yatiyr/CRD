// bench_ode_krylov_vs_cvode_spgmr — v9-z work-precision row: matrix-free Newton-Krylov vs the gold
// standard. The SAME 2D periodic heat MOL (u_t = nu*lap(u), n = nx*ny) solved by
//   Cerid integrate_bdf + KrylovOdeLinearSolver (FGMRES over jacobian_vector, NO Jacobian assembled)
//   vs   SUNDIALS CVODE (CV_BDF) + SUNLinSol_SPGMR (matrix-free, analytic J*v via CVodeSetJacTimes)
// Both matrix-free BDF + GMRES, both with the analytic J*v, both UNPRECONDITIONED (apples-to-apples: same
// method, same operator, same conditioning). A smooth Gaussian IC (many modes => non-trivial GMRES). The
// reference is CVODE-SPGMR at rtol 1e-13 (independent of Cerid, tight). Reports min-of-reps wall ms +
// RHS evals + GMRES iterations + achieved max-error, and the MATCHED-ACHIEVED-ACCURACY readout (the honest
// axis: each solver's controller is calibrated differently). Compile (WSL, from repo root) — see
// scripts/run_bench_krylov.sh.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/ode_krylov_solver.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cvode/cvode.h>
#include <nvector/nvector_serial.h>
#include <sundials/sundials_types.h>
#include <sunlinsol/sunlinsol_spgmr.h>

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
    int nx;
    int ny;
    double nu;
    double dx;
    double dy;
    [[nodiscard]] int n() const { return nx * ny; }
};

// nu * 5-point periodic Laplacian of u -> out.
inline void laplacian(const double* u, double* out, const Params& p)
{
    const int nx = p.nx, ny = p.ny;
    const double cx = p.nu / (p.dx * p.dx);
    const double cy = p.nu / (p.dy * p.dy);
    for (int i = 0; i < nx; ++i)
    {
        for (int j = 0; j < ny; ++j)
        {
            const int idx = i * ny + j;
            const double up = u[((i + 1) % nx) * ny + j];
            const double um = u[((i + nx - 1) % nx) * ny + j];
            const double vp = u[i * ny + ((j + 1) % ny)];
            const double vm = u[i * ny + ((j + ny - 1) % ny)];
            out[idx] = cx * (up - 2.0 * u[idx] + um) + cy * (vp - 2.0 * u[idx] + vm);
        }
    }
}

// ---------------- Cerid form (matrix-free: rhs + jacobian_vector only) ----------------
class Heat2D final : public ode::OdeFunction<f64>
{
public:
    explicit Heat2D(Params p) : ode::OdeFunction<f64>(/*jac*/ false, /*jacvec*/ true), p_(p) {}
    void rhs(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override { laplacian(y.data(), d.data(), p_); }
    [[nodiscard]] bool jacobian_vector(f64, cont::ConstSpan<f64>, cont::ConstSpan<f64> v,
                                       cont::Span<f64> jv) const override
    {
        laplacian(v.data(), jv.data(), p_); // J is the (linear) Laplacian
        return true;
    }
    [[nodiscard]] usize dim() const noexcept override { return static_cast<usize>(p_.n()); }

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
        best = ms < best ? ms : best;
    }
    return best;
}

// ---------------- CVODE form ----------------
int cv_rhs(sunrealtype, N_Vector y, N_Vector ydot, void* ud)
{
    laplacian(N_VGetArrayPointer(y), N_VGetArrayPointer(ydot), *static_cast<Params*>(ud));
    return 0;
}
int cv_jtimes(N_Vector v, N_Vector Jv, sunrealtype, N_Vector, N_Vector, void* ud, N_Vector)
{
    laplacian(N_VGetArrayPointer(v), N_VGetArrayPointer(Jv), *static_cast<Params*>(ud));
    return 0;
}

// One CVODE-SPGMR solve to t1; fills y_out (size n), returns wall handled by caller. Counters via out-params.
void cvode_spgmr_solve(Params& p, const cont::Array<f64>& u0, double rtol, double t1, cont::Array<f64>& y_out,
                       long& nfev, long& nliters)
{
    const int n = p.n();
    SUNContext ctx;
    SUNContext_Create(nullptr, &ctx);
    N_Vector y = N_VNew_Serial(n, ctx);
    double* yd = N_VGetArrayPointer(y);
    for (int i = 0; i < n; ++i)
        yd[i] = u0[i];
    void* mem = CVodeCreate(CV_BDF, ctx);
    CVodeInit(mem, cv_rhs, 0.0, y);
    CVodeSStolerances(mem, rtol, rtol * 1e-3);
    CVodeSetUserData(mem, &p);
    SUNLinearSolver LS = SUNLinSol_SPGMR(y, SUN_PREC_NONE, 0, ctx); // 0 = default maxl=5; matrix-free
    CVodeSetLinearSolver(mem, LS, nullptr);
    CVodeSetJacTimes(mem, nullptr, cv_jtimes); // analytic J*v
    sunrealtype t = 0.0;
    CVode(mem, t1, y, &t, CV_NORMAL);
    CVodeGetNumRhsEvals(mem, &nfev);
    CVodeGetNumLinIters(mem, &nliters);
    double* yo = N_VGetArrayPointer(y);
    y_out.resize(n);
    for (int i = 0; i < n; ++i)
        y_out[i] = yo[i];
    CVodeFree(&mem);
    SUNLinSolFree(LS);
    N_VDestroy(y);
    SUNContext_Free(&ctx);
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 28);
    Params p{32, 32, 1.0, (2.0 * kPi) / 32.0, (2.0 * kPi) / 32.0};
    const int n = p.n();
    const double t1 = 0.02;
    Heat2D f(p);

    // Smooth Gaussian IC (many Fourier modes => non-trivial GMRES).
    cont::Array<f64> u0(&alloc);
    u0.resize(n);
    for (int i = 0; i < p.nx; ++i)
        for (int j = 0; j < p.ny; ++j)
        {
            const double x = i * p.dx, yv = j * p.dy;
            u0[i * p.ny + j] = std::exp(-((x - kPi) * (x - kPi) + (yv - kPi) * (yv - kPi)));
        }

    // Reference: CVODE-SPGMR at rtol 1e-13.
    cont::Array<f64> uref(&alloc);
    {
        long a, b;
        cvode_spgmr_solve(p, u0, 1e-13, t1, uref, a, b);
    }
    auto maxerr = [&](const cont::Array<f64>& u)
    {
        double m = 0.0;
        for (int i = 0; i < n; ++i)
            m = std::max(m, std::abs(u[i] - uref[i]));
        return m;
    };

    std::printf("=== matrix-free Krylov: 2D heat MOL N=%d (%dx%d), nu=%.1f, t=%.3f (best of 10) ===\n", n, p.nx, p.ny,
                p.nu, t1);
    std::printf("\n-- work-precision sweep (err / ms / nfev / GMRES-iters) --\n");
    std::printf("%-7s | %-32s | %-32s\n", "rtol", "Cerid BDF+Krylov", "CVODE BDF+SPGMR");

    const double rtols[] = {1e-4, 1e-5, 1e-6, 1e-7, 1e-8, 1e-9};
    constexpr int kN = 6;
    double c_err[kN], c_ms[kN], a_err[kN], a_ms[kN];
    long c_fev[kN], c_it[kN], a_fev[kN], a_it[kN];

    for (int k = 0; k < kN; ++k)
    {
        const double rtol = rtols[k];
        // Cerid matrix-free BDF.
        cont::Array<f64> uc(&alloc);
        uc.resize(n);
        ode::KrylovOdeLinearSolver<f64> krylov(&alloc, /*restart*/ 60, 1e-7, 2000);
        crd::u64 fev = 0;
        crd::u64 it = 0;
        c_ms[k] = time_best_ms(10,
                               [&]
                               {
                                   for (int i = 0; i < n; ++i)
                                       uc[i] = u0[i];
                                   ode::KrylovOdeLinearSolver<f64> ks(&alloc, 60, 0.05, 2000);
                                   ode::OdeOptions<f64> o;
                                   o.rtol = rtol;
                                   o.atol = rtol * 1e-3;
                                   const auto r =
                                       ode::integrate_bdf<f64>(f, 0.0, t1, cont::Span<f64>(uc.data(), n), o, &alloc, &ks);
                                   fev = r.work.nfev;
                                   it = ks.total_gmres_iterations();
                               });
        c_fev[k] = static_cast<long>(fev);
        c_it[k] = static_cast<long>(it);
        c_err[k] = maxerr(uc);

        // CVODE-SPGMR.
        cont::Array<f64> ua(&alloc);
        long afev = 0, ait = 0;
        a_ms[k] = time_best_ms(10,
                               [&]
                               {
                                   long fe, li;
                                   cvode_spgmr_solve(p, u0, rtol, t1, ua, fe, li);
                                   afev = fe;
                                   ait = li;
                               });
        a_fev[k] = afev;
        a_it[k] = ait;
        a_err[k] = maxerr(ua);

        std::printf("%-7.0e | %.2e/%6.2fms/%4ld fev/%4ld it | %.2e/%6.2fms/%4ld fev/%4ld it\n", rtol, c_err[k],
                    c_ms[k], c_fev[k], c_it[k], a_err[k], a_ms[k], a_fev[k], a_it[k]);
    }

    std::printf("\n-- matched ACHIEVED accuracy (work-optimal point reaching the target err) --\n");
    std::printf("%-10s | %-22s | %-22s | crush\n", "target err", "Cerid (ms/fev/it)", "CVODE (ms/fev/it)");
    for (double target : {1e-4, 1e-6, 1e-8})
    {
        int ci = -1, ai = -1;
        for (int k = 0; k < kN; ++k)
            if (c_err[k] <= target && (ci < 0 || c_ms[k] < c_ms[ci]))
                ci = k;
        for (int k = 0; k < kN; ++k)
            if (a_err[k] <= target && (ai < 0 || a_ms[k] < a_ms[ai]))
                ai = k;
        if (ci < 0 || ai < 0)
        {
            std::printf("%-10.0e | (not reached in sweep)\n", target);
            continue;
        }
        std::printf("%-10.0e | %5.2fms/%4ld/%4ld     | %5.2fms/%4ld/%4ld     | %.2fx wall\n", target, c_ms[ci],
                    c_fev[ci], c_it[ci], a_ms[ai], a_fev[ai], a_it[ai], a_ms[ai] / c_ms[ci]);
    }
    return 0;
}
