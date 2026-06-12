// ode_scipy_difftest — v9-b: print Cerid integrate_erk step-sequence numbers (naccept / nreject / nfev /
// final state at 17 sig digits) on the problems scripts/ode_scipy_ref.py runs with scipy solve_ivp at
// identical options. The semantics port is verbatim (rk.py read line-by-line), so step COUNTS must match
// exactly; final states agree to vectorization-order roundoff (numpy pairwise sums vs our serial loops).
// Run both, compare, record in the session log (the v7 NM-219=219 playbook).

#include <crd/containers/array.hpp>
#include <crd/hesap/ode/bdf.hpp>
#include <crd/hesap/ode/erk.hpp>
#include <crd/hesap/ode/radau.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>

using crd::f64;
namespace ode = crd::hesap::ode;
namespace containers = crd::containers;

namespace
{

void run_case(const char* name, ode::ErkMethod method, const ode::OdeFunction<f64>& f, f64 t0, f64 t1, const f64* y0,
              crd::usize n, f64 rtol, f64 atol, crd::memory::IAllocator* alloc)
{
    containers::Array<f64> y(alloc);
    y.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        y[i] = y0[i];
    }
    ode::OdeOptions<f64> opts;
    opts.rtol = rtol;
    opts.atol = atol;
    const ode::OdeResult<f64> r =
        ode::integrate_erk<f64>(f, t0, t1, containers::Span<f64>(y.data(), n), opts, alloc, method);
    std::printf("%s: status=%d naccept=%llu nreject=%llu nfev=%llu y=[", name, static_cast<int>(r.status),
                static_cast<unsigned long long>(r.work.naccept), static_cast<unsigned long long>(r.work.nreject),
                static_cast<unsigned long long>(r.work.nfev));
    for (crd::usize i = 0; i < n; ++i)
    {
        std::printf("%s%.17g", i ? ", " : "", y[i]);
    }
    std::printf("]\n");
}

} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1U << 22);

    // Van der Pol mu = 1, y0 = (2, 0), t in [0, 10].
    const auto vdp = ode::make_ode_function<f64>(2,
                                                 [](f64, containers::ConstSpan<f64> y, containers::Span<f64> d)
                                                 {
                                                     d[0] = y[1];
                                                     d[1] = (1.0 - y[0] * y[0]) * y[1] - y[0];
                                                 });
    // Exponential decay, y0 = 1, t in [0, 5].
    const auto dec = ode::make_ode_function<f64>(1, [](f64, containers::ConstSpan<f64> y, containers::Span<f64> d)
                                                 { d[0] = -y[0]; });

    const f64 vdp0[2] = {2.0, 0.0};
    const f64 dec0[1] = {1.0};

    run_case("vdp_rk23", ode::ErkMethod::Rk23, vdp, 0.0, 10.0, vdp0, 2, 1e-8, 1e-8, &alloc);
    run_case("vdp_rk45", ode::ErkMethod::Rk45, vdp, 0.0, 10.0, vdp0, 2, 1e-8, 1e-8, &alloc);
    run_case("vdp_dop853", ode::ErkMethod::Dop853, vdp, 0.0, 10.0, vdp0, 2, 1e-8, 1e-8, &alloc);
    run_case("dec_rk45", ode::ErkMethod::Rk45, dec, 0.0, 5.0, dec0, 1, 1e-10, 1e-12, &alloc);
    run_case("dec_dop853", ode::ErkMethod::Dop853, dec, 0.0, 5.0, dec0, 1, 1e-10, 1e-12, &alloc);

    // --- v9-d: BDF with ANALYTIC Jacobians (scipy jac=callable — the trajectory-exact configuration) ---
    class Robertson final : public ode::OdeFunction<f64>
    {
    public:
        Robertson() : ode::OdeFunction<f64>(true) {}
        void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
        {
            d[0] = -0.04 * y[0] + 1e4 * y[1] * y[2];
            d[1] = 0.04 * y[0] - 1e4 * y[1] * y[2] - 3e7 * y[1] * y[1];
            d[2] = 3e7 * y[1] * y[1];
        }
        [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64> y, containers::Span<f64> j) const override
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
    class VdpStiff final : public ode::OdeFunction<f64>
    {
    public:
        VdpStiff() : ode::OdeFunction<f64>(true) {}
        void rhs(f64, containers::ConstSpan<f64> y, containers::Span<f64> d) const override
        {
            d[0] = y[1];
            d[1] = 1000.0 * ((1.0 - y[0] * y[0]) * y[1]) - y[0];
        }
        [[nodiscard]] bool jacobian(f64, containers::ConstSpan<f64> y, containers::Span<f64> j) const override
        {
            j[0] = 0.0;
            j[1] = 1.0;
            j[2] = 1000.0 * (-2.0 * y[0] * y[1]) - 1.0;
            j[3] = 1000.0 * (1.0 - y[0] * y[0]);
            return true;
        }
        [[nodiscard]] crd::usize dim() const noexcept override { return 2; }
    };

    auto run_bdf = [&alloc](const char* name, const ode::OdeFunction<f64>& f, f64 t0, f64 t1, const f64* y0,
                            crd::usize n, f64 rtol, f64 atol)
    {
        containers::Array<f64> y(&alloc);
        y.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = y0[i];
        }
        ode::OdeOptions<f64> opts;
        opts.rtol = rtol;
        opts.atol = atol;
        const ode::OdeResult<f64> r =
            ode::integrate_bdf<f64>(f, t0, t1, containers::Span<f64>(y.data(), n), opts, &alloc);
        std::printf("%s: status=%d naccept=%llu nfev=%llu njev=%llu nlu=%llu y=[", name, static_cast<int>(r.status),
                    static_cast<unsigned long long>(r.work.naccept), static_cast<unsigned long long>(r.work.nfev),
                    static_cast<unsigned long long>(r.work.njev), static_cast<unsigned long long>(r.work.nlu));
        for (crd::usize i = 0; i < n; ++i)
        {
            std::printf("%s%.17g", i ? ", " : "", y[i]);
        }
        std::printf("]\n");
    };

    Robertson rob;
    VdpStiff vdps;
    const f64 rob0[3] = {1.0, 0.0, 0.0};
    run_bdf("rober_bdf", rob, 0.0, 100.0, rob0, 3, 1e-6, 1e-10);
    run_bdf("vdp1000_bdf", vdps, 0.0, 300.0, vdp0, 2, 1e-6, 1e-8);

    // --- v9-e: Radau with analytic Jacobians ---
    auto run_radau = [&alloc](const char* name, const ode::OdeFunction<f64>& f, f64 t0, f64 t1, const f64* y0,
                              crd::usize n, f64 rtol, f64 atol)
    {
        containers::Array<f64> y(&alloc);
        y.resize(n);
        for (crd::usize i = 0; i < n; ++i)
        {
            y[i] = y0[i];
        }
        ode::OdeOptions<f64> opts;
        opts.rtol = rtol;
        opts.atol = atol;
        const ode::OdeResult<f64> r =
            ode::integrate_radau<f64>(f, t0, t1, containers::Span<f64>(y.data(), n), opts, &alloc);
        std::printf("%s: status=%d naccept=%llu nfev=%llu njev=%llu nlu=%llu y=[", name, static_cast<int>(r.status),
                    static_cast<unsigned long long>(r.work.naccept), static_cast<unsigned long long>(r.work.nfev),
                    static_cast<unsigned long long>(r.work.njev), static_cast<unsigned long long>(r.work.nlu));
        for (crd::usize i = 0; i < n; ++i)
        {
            std::printf("%s%.17g", i ? ", " : "", y[i]);
        }
        std::printf("]\n");
    };
    run_radau("rober_radau", rob, 0.0, 100.0, rob0, 3, 1e-6, 1e-10);
    run_radau("vdp1000_radau", vdps, 0.0, 300.0, vdp0, 2, 1e-6, 1e-8);
    return 0;
}
