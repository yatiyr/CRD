// crd-hesap-quadrature v13-g bench — integrate (precomputed Gauss) + composite trapezoid/Simpson + Newton-Cotes,
// repeated-call (real-time) timing. Peers: scipy.integrate (fixed_quad/simpson/trapezoid/newton_cotes), Boost
// gauss<n>, MATLAB trapz.

#include <crd/hesap/quadrature/quadrature.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

int main()
{
    namespace q = crd::hesap::quadrature;
    using crd::containers::Array;
    using crd::containers::ConstSpan;
    using clk = std::chrono::steady_clock;
    crd::memory::TlsfAllocator alloc(1U << 22);
    double                     acc = 0.0;
    auto                       f   = [](double x) { return std::exp(x) * std::sin(2.0 * x); };
    const int                  R   = 200000;

    Array<double> gx(&alloc);
    Array<double> gw(&alloc);
    gx.resize(10);
    gw.resize(10);
    q::gauss_legendre<double>(&alloc, 10, gx.data(), gw.data());
    {
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_with_nodes<double>(f, 0.0, 2.0, ConstSpan<double>{gx.data(), 10},
                                                   ConstSpan<double>{gw.data(), 10})
                       .value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "gauss10 (precomp)",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }

    {
        // symmetric-pair fast path (Gauss-Legendre nodes ±xᵢ, symmetric weights): ⌈n/2⌉ weight-mults.
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_symmetric<double>(f, 0.0, 2.0, ConstSpan<double>{gx.data(), 10},
                                                  ConstSpan<double>{gw.data(), 10})
                       .value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "gauss10 (sym)",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }

    const int     n = 1001;
    Array<double> y(&alloc);
    y.resize(static_cast<crd::usize>(n));
    const double dx = 2.0 / (n - 1);
    for (int i = 0; i < n; ++i)
    {
        y[static_cast<crd::usize>(i)] = f(i * dx);
    }
    {
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::simpson<double>(ConstSpan<double>{y.data(), static_cast<crd::usize>(n)}, dx);
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "simpson n=1001",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::trapezoid<double>(ConstSpan<double>{y.data(), static_cast<crd::usize>(n)}, dx);
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "trapezoid n=1001",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        Array<double> ncw(&alloc);
        ncw.resize(9);
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            q::newton_cotes<double>(&alloc, 8, ncw.data());
            acc += ncw[0];
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "newton_cotes(8)",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_adaptive<double>(&alloc, f, 0.0, 1.0, 1e-10, 1e-10, 50).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "adaptive QAG [0,1]",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        q::AdaptiveWorkspace<double> ws(&alloc, 50); // reused across calls (the fair, real-world path — like GSL)
        auto                         gs = [](double x) { return 1.0 / std::sqrt(x); };
        const auto                   t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_qags<double>(ws, gs, 0.0, 1.0, 1e-10, 1e-10).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "QAGS 1/sqrt(x) [ws]",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        q::AdaptiveWorkspace<double> ws(&alloc, 50);
        auto                         gi = [](double x) { return std::exp(-x * x); };
        const auto                   t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_qagi<double>(ws, gi, 0.0, 1, 1e-10, 1e-10).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "QAGI e^-x^2 [ws]",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        auto       fn = [](double x) { return std::exp(x) * std::cos(2.0 * x); };
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_qng<double>(fn, 0.0, 1.0, 1e-10, 1e-10).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "QNG e^x*cos2x [smooth]",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        q::AdaptiveWorkspace<double> ws(&alloc, 50);
        double                       bp[] = {1.0};
        auto                         fp   = [](double x) { return 1.0 / std::sqrt(std::abs(x - 1.0)); };
        const auto                   t0   = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_qagp<double>(ws, fp, 0.0, 3.0, ConstSpan<double>{bp, 1}, 1e-9, 1e-9).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "QAGP |x-1|^-.5 bp=1",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        const auto rule = q::build_exp_sinh_rule<double>(&alloc); // precomputed once, reused (the crush lever)
        auto       gi   = [](double x) { return std::exp(-x * x); };
        const auto t0   = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_exp_sinh<double>(rule, gi, 0.0, 1e-12, 1e-12).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "DE exp_sinh e^-x^2",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        const auto rule = q::build_sinh_sinh_rule<double>(&alloc);
        auto       gi   = [](double x) { return std::exp(-x * x); };
        const auto t0   = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_sinh_sinh<double>(rule, gi, 1e-12, 1e-12).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "DE sinh_sinh e^-x^2",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        const auto rule = q::build_tanh_sinh_rule<double>(&alloc);
        auto       gs   = [](double x) { return 1.0 / std::sqrt(x); };
        const auto t0   = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_tanh_sinh<double>(rule, gs, 0.0, 1.0, 1e-12, 1e-12).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "DE tanh_sinh 1/sqrt(x)",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        auto       fn = [](double x) { return std::exp(x) * std::cos(2.0 * x); };
        const auto t0 = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_romberg<double>(fn, 0.0, 1.0, 1e-10, 1e-10).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "Romberg e^x*cos2x",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    {
        const auto rule = q::build_cc_adaptive_rule<double>(&alloc); // precomputed weights, reused
        auto       fn   = [](double x) { return std::exp(x) * std::cos(2.0 * x); };
        const auto t0   = clk::now();
        for (int r = 0; r < R; ++r)
        {
            acc += q::integrate_clenshaw_curtis<double>(rule, &alloc, fn, 0.0, 1.0, 1e-10, 1e-10).value;
        }
        const auto t1 = clk::now();
        std::printf("%-22s %7.1f ns/call\n", "CC-adaptive e^x*cos2x",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / R);
    }
    std::printf("(checksum %.6f)\n", acc);
    return 0;
}
