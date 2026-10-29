// bench_ode_vs_refs: the hesap-ode board against the same-language gold standards, one thread.
//   STIFF:    Cerid BDF / Radau / RODAS4 / TR-BDF2  vs  SUNDIALS CVODE (BDF + dense LU + analytic jac)
//             on ROBER (t = 1e5) and Van der Pol mu = 1000 (t = 300), matched rtol/atol; heat-2D n = 4096, Cerid's
//             sparse BDF vs CVODE + KLU, analytic sparse Jacobians on both sides.
//   NONSTIFF: Cerid RK45 / DOP853 vs Boost.Odeint dopri5 (controlled) on VdP mu = 1 (t = 100); odeint runs in the
//             crd-bench-boost-peer library at the host's ISA (examples/peers/boost_peer.hpp), one call per run.
// Reports min-of-reps wall ms + nfev + the achieved |error| vs a tight-tolerance reference, so cross-method rows are
// judged at ACHIEVED accuracy, not just matched tolerance settings (the honest scoreboard rule). Built when both
// CRD_BUILD_HESAP_VS_SUNDIALS and CRD_BUILD_HESAP_VS_BOOST are on; scripts/run_bench_ode_refs.sh runs it per CPU level.

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/ode_sparse_solver.hpp>
#include <crd/hesap/ode/radau.hpp>
#include <crd/hesap/ode/rosenbrock.hpp>
#include <crd/hesap/ode/sdirk.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include "boost_peer.hpp"

#include <sunlinsol/sunlinsol_klu.h>
#include <sunmatrix/sunmatrix_sparse.h>

#include <cvode/cvode.h>
#include <nvector/nvector_serial.h>
#include <sundials/sundials_types.h>
#include <sunlinsol/sunlinsol_dense.h>
#include <sunmatrix/sunmatrix_dense.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>

using crd::f64;
namespace ode = crd::hesap::ode;
namespace cont = crd::containers;

namespace
{

f64 g_ref_rober[3];
f64 g_ref_vdp1000[2];
f64 g_ref_vdp1[2];

// ---------------- problems (Cerid form) ----------------
class Robertson final : public ode::OdeFunction<f64>
{
public:
    Robertson() : ode::OdeFunction<f64>(true) {}
    void rhs(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override
    {
        d[0] = -0.04 * y[0] + 1e4 * y[1] * y[2];
        d[1] = 0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] * y[1];
        d[2] = 3e7 * y[1] * y[1];
    }
    [[nodiscard]] bool jacobian(f64, cont::ConstSpan<f64> y, cont::Span<f64> j) const override
    {
        j[0] = -0.04;
        j[1] = 1e4 * y[2];
        j[2] = 1e4 * y[1];
        j[3] = 0.04;
        j[4] = -1e4 * y[2] - 6e7 * y[1];
        j[5] = -1e4 * y[1];
        j[6] = 0.0;
        j[7] = 6e7 * y[1];
        j[8] = 0.0;
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return 3; }
};

class Vdp final : public ode::OdeFunction<f64>
{
public:
    explicit Vdp(f64 mu) : ode::OdeFunction<f64>(true), m_mu(mu) {}
    void rhs(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override
    {
        d[0] = y[1];
        d[1] = m_mu * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
    }
    [[nodiscard]] bool jacobian(f64, cont::ConstSpan<f64> y, cont::Span<f64> j) const override
    {
        j[0] = 0.0;
        j[1] = 1.0;
        j[2] = m_mu * (-2.0 * y[0] * y[1]) - 1.0;
        j[3] = m_mu * (1.0 - y[0] * y[0]);
        return true;
    }
    [[nodiscard]] crd::usize dim() const noexcept override { return 2; }

private:
    f64 m_mu;
};

f64 err_vs_ref(const f64* y, const f64* ref, crd::usize n)
{
    f64 e = 0.0;
    for (crd::usize i = 0; i < n; ++i)
    {
        const f64 d = std::abs(y[i] - ref[i]);
        e = d > e ? d : e;
    }
    return e;
}

template <typename Fn> double time_best_ms(int reps, const Fn& fn)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const auto a = std::chrono::steady_clock::now();
        fn();
        const auto b = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(b - a).count();
        best = ms < best ? ms : best;
    }
    return best;
}

enum class Method : std::uint8_t
{
    Bdf,
    Radau,
    Rodas4,
    Trbdf2,
    Rk45,
    Dop853
};

void bench_cerid(const char* label, Method m, const ode::OdeFunction<f64>& f, f64 t0, f64 t1, const f64* y0,
                 crd::usize n, f64 rtol, f64 atol, const f64* ref, int reps, crd::memory::IAllocator* alloc)
{
    cont::Array<f64> y(alloc);
    y.resize(n);
    ode::OdeResult<f64> r;
    const double ms = time_best_ms(reps,
                                   [&]
                                   {
                                       for (crd::usize i = 0; i < n; ++i)
                                       {
                                           y[i] = y0[i];
                                       }
                                       ode::OdeOptions<f64> o;
                                       o.rtol = rtol;
                                       o.atol = atol;
                                       const cont::Span<f64> ys(y.data(), n);
                                       switch (m)
                                       {
                                           case Method::Bdf:
                                               r = ode::integrate_bdf<f64>(f, t0, t1, ys, o, alloc);
                                               break;
                                           case Method::Radau:
                                               r = ode::integrate_radau<f64>(f, t0, t1, ys, o, alloc);
                                               break;
                                           case Method::Rodas4:
                                               r = ode::integrate_rosenbrock<f64>(f, t0, t1, ys, o, alloc);
                                               break;
                                           case Method::Trbdf2:
                                               r = ode::integrate_trbdf2<f64>(f, t0, t1, ys, o, alloc);
                                               break;
                                           case Method::Rk45:
                                               r = ode::integrate_erk<f64>(f, t0, t1, ys, o, alloc,
                                                                           ode::ErkMethod::Rk45);
                                               break;
                                           case Method::Dop853:
                                               r = ode::integrate_erk<f64>(f, t0, t1, ys, o, alloc,
                                                                           ode::ErkMethod::Dop853);
                                               break;
                                       }
                                   });
    std::printf("%-22s %9.3f ms  nfev=%-7llu nlu=%-5llu err=%.2e %s\n", label, ms,
                static_cast<unsigned long long>(r.work.nfev), static_cast<unsigned long long>(r.work.nlu),
                err_vs_ref(y.data(), ref, n), r.success ? "" : "FAIL");
}

// ---------------- CVODE side ----------------
struct CvProblem
{
    int which; // 0 = rober, 1 = vdp1000
};

int cv_rhs(sunrealtype, N_Vector yv, N_Vector fv, void* user)
{
    const auto* p = static_cast<CvProblem*>(user);
    const sunrealtype* y = N_VGetArrayPointer(yv);
    sunrealtype* f = N_VGetArrayPointer(fv);
    if (p->which == 0)
    {
        f[0] = -0.04 * y[0] + 1e4 * y[1] * y[2];
        f[1] = 0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] * y[1];
        f[2] = 3e7 * y[1] * y[1];
    }
    else
    {
        f[0] = y[1];
        f[1] = 1000.0 * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
    }
    return 0;
}

int cv_jac(sunrealtype, N_Vector yv, N_Vector, SUNMatrix J, void* user, N_Vector, N_Vector, N_Vector)
{
    const auto* p = static_cast<CvProblem*>(user);
    const sunrealtype* y = N_VGetArrayPointer(yv);
    sunrealtype* j = SUNDenseMatrix_Data(J); // column-major
    if (p->which == 0)
    {
        j[0] = -0.04;
        j[1] = 0.04;
        j[2] = 0.0;
        j[3] = 1e4 * y[2];
        j[4] = -1e4 * y[2] - 6e7 * y[1];
        j[5] = 6e7 * y[1];
        j[6] = 1e4 * y[1];
        j[7] = -1e4 * y[1];
        j[8] = 0.0;
    }
    else
    {
        j[0] = 0.0;
        j[1] = 1000.0 * (-2.0 * y[0] * y[1]) - 1.0;
        j[2] = 1.0;
        j[3] = 1000.0 * (1.0 - y[0] * y[0]);
    }
    return 0;
}

void bench_cvode(const char* label, int which, f64 t1, const f64* y0, crd::usize n, f64 rtol, f64 atol,
                 const f64* ref, int reps)
{
    SUNContext ctx;
    SUNContext_Create(nullptr, &ctx); // SUNDIALS 6.x signature (SUN_COMM_NULL arrives in 7.x)
    CvProblem prob{which};
    long nfev_out = 0;
    long nlu_out = 0;
    N_Vector y = N_VNew_Serial(static_cast<sunindextype>(n), ctx);
    SUNMatrix jacobian = SUNDenseMatrix(static_cast<sunindextype>(n), static_cast<sunindextype>(n), ctx);
    f64 yfin[3] = {0, 0, 0};

    const double ms = time_best_ms(
        reps,
        [&]
        {
            for (crd::usize i = 0; i < n; ++i)
            {
                N_VGetArrayPointer(y)[i] = y0[i];
            }
            void* mem = CVodeCreate(CV_BDF, ctx);
            CVodeInit(mem, cv_rhs, 0.0, y);
            CVodeSStolerances(mem, rtol, atol);
            CVodeSetUserData(mem, &prob);
            SUNLinearSolver solver = SUNLinSol_Dense(y, jacobian, ctx);
            CVodeSetLinearSolver(mem, solver, jacobian);
            CVodeSetJacFn(mem, cv_jac);
            CVodeSetMaxNumSteps(mem, 10000000);
            sunrealtype tret = 0;
            CVode(mem, t1, y, &tret, CV_NORMAL);
            long nf = 0;
            CVodeGetNumRhsEvals(mem, &nf);
            long nsetups = 0;
            CVodeGetNumLinSolvSetups(mem, &nsetups);
            nfev_out = nf;
            nlu_out = nsetups;
            for (crd::usize i = 0; i < n; ++i)
            {
                yfin[i] = N_VGetArrayPointer(y)[i];
            }
            SUNLinSolFree(solver);
            CVodeFree(&mem);
        });
    std::printf("%-22s %9.3f ms  nfev=%-7ld nlu=%-5ld err=%.2e\n", label, ms, nfev_out, nlu_out,
                err_vs_ref(yfin, ref, n));
    SUNMatDestroy(jacobian);
    N_VDestroy(y);
    SUNContext_Free(&ctx);
}

// ---------------- odeint side (nonstiff dopri5) ----------------
void bench_odeint_dopri5(const char* label, f64 mu, f64 t1, f64 rtol, f64 atol, const f64* ref, int reps)
{
    const crd_peer::boost_odeint::VanDerPol problem{mu, t1, {2.0, 0.0}};
    const crd_peer::boost_odeint::Tolerances tolerances{atol, rtol, 1e-4};
    crd_peer::boost_odeint::Integration run{};
    const double ms = time_best_ms(reps, [&] { run = crd_peer::boost_odeint::dopri5(problem, tolerances); });
    std::printf("%-22s %9.3f ms  nfev=%-7lld %*s err=%.2e\n", label, ms, static_cast<long long>(run.rhs_evaluations),
                10, "", err_vs_ref(run.y, ref, 2));
}

} // namespace

int main()
{
    std::printf("# Boost.Odeint peer: %s\n", crd_peer::boost_math::isa());
    crd::memory::TlsfAllocator alloc(1U << 24);
    Robertson rober;
    Vdp vdp1000(1000.0);
    Vdp vdp1(1.0);

    // Tight-tolerance Radau references (achieved-accuracy anchors).
    const f64 rob0[3] = {1.0, 0.0, 0.0};
    const f64 vdp0[2] = {2.0, 0.0};
    {
        cont::Array<f64> y(&alloc);
        y.resize(3);
        y[0] = 1.0;
        y[1] = 0.0;
        y[2] = 0.0;
        ode::OdeOptions<f64> o;
        o.rtol = 1e-12;
        o.atol = 1e-14;
        (void)ode::integrate_radau<f64>(rober, 0.0, 1e5, cont::Span<f64>(y.data(), 3), o, &alloc);
        for (int i = 0; i < 3; ++i)
        {
            g_ref_rober[i] = y[i];
        }
        cont::Array<f64> z(&alloc);
        z.resize(2);
        z[0] = 2.0;
        z[1] = 0.0;
        (void)ode::integrate_radau<f64>(vdp1000, 0.0, 300.0, cont::Span<f64>(z.data(), 2), o, &alloc);
        g_ref_vdp1000[0] = z[0];
        g_ref_vdp1000[1] = z[1];
        z[0] = 2.0;
        z[1] = 0.0;
        ode::OdeOptions<f64> o2;
        o2.rtol = 1e-13;
        o2.atol = 1e-14;
        (void)ode::integrate_erk<f64>(vdp1, 0.0, 100.0, cont::Span<f64>(z.data(), 2), o2, &alloc,
                                      ode::ErkMethod::Dop853);
        g_ref_vdp1[0] = z[0];
        g_ref_vdp1[1] = z[1];
    }

    std::printf("=== STIFF: ROBER t=1e5, rtol=1e-6 atol=1e-10, analytic jac (reps: best of 30) ===\n");
    bench_cerid("cerid_bdf", Method::Bdf, rober, 0.0, 1e5, rob0, 3, 1e-6, 1e-10, g_ref_rober, 30, &alloc);
    bench_cerid("cerid_radau", Method::Radau, rober, 0.0, 1e5, rob0, 3, 1e-6, 1e-10, g_ref_rober, 30, &alloc);
    bench_cerid("cerid_rodas4", Method::Rodas4, rober, 0.0, 1e5, rob0, 3, 1e-6, 1e-10, g_ref_rober, 30, &alloc);
    bench_cerid("cerid_trbdf2", Method::Trbdf2, rober, 0.0, 1e5, rob0, 3, 1e-6, 1e-10, g_ref_rober, 30, &alloc);
    bench_cvode("cvode_bdf", 0, 1e5, rob0, 3, 1e-6, 1e-10, g_ref_rober, 30);

    std::printf("\n=== STIFF: VdP mu=1000 t=300, rtol=1e-6 atol=1e-8, analytic jac (best of 30) ===\n");
    bench_cerid("cerid_bdf", Method::Bdf, vdp1000, 0.0, 300.0, vdp0, 2, 1e-6, 1e-8, g_ref_vdp1000, 30, &alloc);
    bench_cerid("cerid_radau", Method::Radau, vdp1000, 0.0, 300.0, vdp0, 2, 1e-6, 1e-8, g_ref_vdp1000, 30,
                &alloc);
    bench_cerid("cerid_rodas4", Method::Rodas4, vdp1000, 0.0, 300.0, vdp0, 2, 1e-6, 1e-8, g_ref_vdp1000, 30,
                &alloc);
    bench_cerid("cerid_trbdf2", Method::Trbdf2, vdp1000, 0.0, 300.0, vdp0, 2, 1e-6, 1e-8, g_ref_vdp1000, 30,
                &alloc);
    bench_cvode("cvode_bdf", 1, 300.0, vdp0, 2, 1e-6, 1e-8, g_ref_vdp1000, 30);

    // ===== v9-j: LARGE-n MOL — heat-2D 64x64 (n = 4096), SPARSE vs SPARSE (Cerid multifrontal vs
    // CVODE-KLU), analytic sparse Jacobians both sides, eigenmode IC so the achieved error is exact =====
    {
        const crd::u32 m = 64;
        const crd::usize nn = static_cast<crd::usize>(m) * m;
        const f64 inv_dx2 = static_cast<f64>(m + 1) * static_cast<f64>(m + 1);
        const f64 pi = 3.14159265358979323846;
        const f64 dx = 1.0 / static_cast<f64>(m + 1);
        const f64 s = std::sin(pi * dx / 2.0);
        const f64 lambda = -8.0 * inv_dx2 * s * s;
        const f64 t_end = 0.05;
        const f64 decay = std::exp(lambda * t_end);

        // Cerid side: the heat OdeFunction (sparse-only).
        class Heat final : public ode::OdeFunction<f64>
        {
        public:
            Heat(crd::u32 mm, f64 idx2, crd::memory::IAllocator* a) : m_m(mm), m_inv(idx2), m_alloc(a)
            {
                set_has_sparse_jacobian(true);
            }
            void rhs(f64, cont::ConstSpan<f64> y, cont::Span<f64> d) const override
            {
                const crd::u32 side = m_m;
                for (crd::u32 i = 0; i < side; ++i)
                {
                    for (crd::u32 j = 0; j < side; ++j)
                    {
                        const crd::usize k = static_cast<crd::usize>(i) * side + j;
                        const f64 up = (i > 0) ? y[k - side] : 0.0;
                        const f64 dn = (i + 1 < side) ? y[k + side] : 0.0;
                        const f64 lf = (j > 0) ? y[k - 1] : 0.0;
                        const f64 rt = (j + 1 < side) ? y[k + 1] : 0.0;
                        d[k] = m_inv * (up + dn + lf + rt - 4.0 * y[k]);
                    }
                }
            }
            [[nodiscard]] bool sparse_jacobian(
                f64, cont::ConstSpan<f64>,
                crd::hesap::sparse::SparseMatrix<f64, crd::hesap::sparse::SparseFormat::Csr>& out) const override
            {
                const crd::u32 side = m_m;
                crd::hesap::sparse::TripletBuilder<f64> tb(m_alloc, side * side, side * side);
                for (crd::u32 i = 0; i < side; ++i)
                {
                    for (crd::u32 j = 0; j < side; ++j)
                    {
                        const crd::u32 k = i * side + j;
                        tb.add(k, k, -4.0 * m_inv);
                        if (i > 0)
                        {
                            tb.add(k, k - side, m_inv);
                        }
                        if (i + 1 < side)
                        {
                            tb.add(k, k + side, m_inv);
                        }
                        if (j > 0)
                        {
                            tb.add(k, k - 1, m_inv);
                        }
                        if (j + 1 < side)
                        {
                            tb.add(k, k + 1, m_inv);
                        }
                    }
                }
                out = tb.compress();
                return true;
            }
            [[nodiscard]] crd::usize dim() const noexcept override
            {
                return static_cast<crd::usize>(m_m) * m_m;
            }

        private:
            crd::u32 m_m;
            f64 m_inv;
            crd::memory::IAllocator* m_alloc;
        } heat(m, inv_dx2, &alloc);

        cont::Array<f64> u0(&alloc);
        u0.resize(nn);
        for (crd::u32 i = 0; i < m; ++i)
        {
            for (crd::u32 j = 0; j < m; ++j)
            {
                u0[static_cast<crd::usize>(i) * m + j] =
                    std::sin(pi * static_cast<f64>(i + 1) * dx) * std::sin(pi * static_cast<f64>(j + 1) * dx);
            }
        }
        const crd::usize mid = (static_cast<crd::usize>(m / 2) * m) + m / 2;

        std::printf("\n=== LARGE-n MOL: heat-2D n=4096, t=0.05, rtol=1e-8 atol=1e-12, SPARSE jac (best of 5) ===\n");
        {
            cont::Array<f64> y(&alloc);
            y.resize(nn);
            ode::OdeResult<f64> r;
            const double ms = time_best_ms(5,
                                           [&]
                                           {
                                               for (crd::usize i = 0; i < nn; ++i)
                                               {
                                                   y[i] = u0[i];
                                               }
                                               ode::SparseOdeLinearSolver<f64> sol(&alloc);
                                               ode::OdeOptions<f64> o;
                                               o.rtol = 1e-8;
                                               o.atol = 1e-12;
                                               r = ode::integrate_bdf<f64>(heat, 0.0, t_end,
                                                                           cont::Span<f64>(y.data(), nn), o,
                                                                           &alloc, &sol);
                                           });
            std::printf("%-22s %9.3f ms  nfev=%-7llu nlu=%-5llu err=%.2e %s\n", "cerid_bdf_sparse", ms,
                        static_cast<unsigned long long>(r.work.nfev),
                        static_cast<unsigned long long>(r.work.nlu), std::abs(y[mid] - decay * u0[mid]),
                        r.success ? "" : "FAIL");
        }
        // CVODE + KLU (CSC; the Laplacian is symmetric so the CSR pattern/values transpose to themselves).
        {
            SUNContext ctx;
            SUNContext_Create(nullptr, &ctx);
            N_Vector y = N_VNew_Serial(static_cast<sunindextype>(nn), ctx);
            const sunindextype nnz = static_cast<sunindextype>(5 * nn);
            SUNMatrix jacobian =
                SUNSparseMatrix(static_cast<sunindextype>(nn), static_cast<sunindextype>(nn), nnz, CSC_MAT, ctx);
            struct HeatCv
            {
                crd::u32 m;
                f64 inv;
            } hc{m, inv_dx2};
            auto rhs_fn = +[](sunrealtype, N_Vector yv, N_Vector fv, void* user) -> int
            {
                const auto* p = static_cast<HeatCv*>(user);
                const sunrealtype* yy = N_VGetArrayPointer(yv);
                sunrealtype* ff = N_VGetArrayPointer(fv);
                const crd::u32 side = p->m;
                for (crd::u32 i = 0; i < side; ++i)
                {
                    for (crd::u32 j = 0; j < side; ++j)
                    {
                        const crd::usize k = static_cast<crd::usize>(i) * side + j;
                        const f64 up = (i > 0) ? yy[k - side] : 0.0;
                        const f64 dn = (i + 1 < side) ? yy[k + side] : 0.0;
                        const f64 lf = (j > 0) ? yy[k - 1] : 0.0;
                        const f64 rt = (j + 1 < side) ? yy[k + 1] : 0.0;
                        ff[k] = p->inv * (up + dn + lf + rt - 4.0 * yy[k]);
                    }
                }
                return 0;
            };
            auto jac_fn = +[](sunrealtype, N_Vector, N_Vector, SUNMatrix J, void* user, N_Vector, N_Vector,
                              N_Vector) -> int
            {
                const auto* p = static_cast<HeatCv*>(user);
                const crd::u32 side = p->m;
                const crd::usize count = static_cast<crd::usize>(side) * side;
                sunindextype* colptr = SUNSparseMatrix_IndexPointers(J);
                sunindextype* rowidx = SUNSparseMatrix_IndexValues(J);
                sunrealtype* vals = SUNSparseMatrix_Data(J);
                crd::usize pos = 0;
                for (crd::usize col = 0; col < count; ++col) // symmetric ⇒ column = row pattern
                {
                    colptr[col] = static_cast<sunindextype>(pos);
                    const crd::u32 i = static_cast<crd::u32>(col / side);
                    const crd::u32 j = static_cast<crd::u32>(col % side);
                    if (i > 0)
                    {
                        rowidx[pos] = static_cast<sunindextype>(col - side);
                        vals[pos++] = p->inv;
                    }
                    if (j > 0)
                    {
                        rowidx[pos] = static_cast<sunindextype>(col - 1);
                        vals[pos++] = p->inv;
                    }
                    rowidx[pos] = static_cast<sunindextype>(col);
                    vals[pos++] = -4.0 * p->inv;
                    if (j + 1 < side)
                    {
                        rowidx[pos] = static_cast<sunindextype>(col + 1);
                        vals[pos++] = p->inv;
                    }
                    if (i + 1 < side)
                    {
                        rowidx[pos] = static_cast<sunindextype>(col + side);
                        vals[pos++] = p->inv;
                    }
                }
                colptr[count] = static_cast<sunindextype>(pos);
                return 0;
            };
            long nfev_out = 0;
            long nlu_out = 0;
            f64 mid_out = 0;
            const double ms = time_best_ms(
                5,
                [&]
                {
                    sunrealtype* yp = N_VGetArrayPointer(y);
                    for (crd::usize i = 0; i < nn; ++i)
                    {
                        yp[i] = u0[i];
                    }
                    void* mem = CVodeCreate(CV_BDF, ctx);
                    CVodeInit(mem, rhs_fn, 0.0, y);
                    CVodeSStolerances(mem, 1e-8, 1e-12);
                    CVodeSetUserData(mem, &hc);
                    SUNLinearSolver solver = SUNLinSol_KLU(y, jacobian, ctx);
                    CVodeSetLinearSolver(mem, solver, jacobian);
                    CVodeSetJacFn(mem, jac_fn);
                    CVodeSetMaxNumSteps(mem, 10000000);
                    sunrealtype tret = 0;
                    CVode(mem, t_end, y, &tret, CV_NORMAL);
                    long nf = 0;
                    long ns = 0;
                    CVodeGetNumRhsEvals(mem, &nf);
                    CVodeGetNumLinSolvSetups(mem, &ns);
                    nfev_out = nf;
                    nlu_out = ns;
                    mid_out = N_VGetArrayPointer(y)[mid];
                    SUNLinSolFree(solver);
                    CVodeFree(&mem);
                });
            std::printf("%-22s %9.3f ms  nfev=%-7ld nlu=%-5ld err=%.2e\n", "cvode_klu", ms, nfev_out, nlu_out,
                        std::abs(mid_out - decay * u0[mid]));
            SUNMatDestroy(jacobian);
            N_VDestroy(y);
            SUNContext_Free(&ctx);
        }
    }

    std::printf("\n=== NONSTIFF: VdP mu=1 t=100, rtol=atol=1e-8 (best of 50) ===\n");
    bench_cerid("cerid_rk45", Method::Rk45, vdp1, 0.0, 100.0, vdp0, 2, 1e-8, 1e-8, g_ref_vdp1, 50, &alloc);
    bench_cerid("cerid_dop853", Method::Dop853, vdp1, 0.0, 100.0, vdp0, 2, 1e-8, 1e-8, g_ref_vdp1, 50, &alloc);
    bench_odeint_dopri5("odeint_dopri5", 1.0, 100.0, 1e-8, 1e-8, g_ref_vdp1, 50);
    return 0;
}
