// v13-a/b interpolation crush bench — BUILD throughput (fit an n-knot interpolant) + EVAL throughput (query at a large
// batch), for PCHIP / cubic-spline / linear, vs scipy.interpolate + MATLAB (see bench_interp_refs.py / _matlab.m).
// Cerid is native C++ over an O(n) Thomas/Fritsch-Carlson build + an O(1)-amortized cached eval; the peers pay the
// Python/MATLAB interpreter + dispatch overhead per call.

#include <crd/hesap/interp/interp.hpp>

#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using namespace crd::hesap::interp;
using crd::containers::ConstSpan;
using crd::f64;
using crd::u64;
using crd::usize;
using clk = std::chrono::steady_clock;

int main()
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 27);
    constexpr usize n = 1000;   // knots
    constexpr usize nq = 100000; // query points
    crd::containers::Array<f64> x(&alloc);
    crd::containers::Array<f64> y(&alloc);
    crd::containers::Array<f64> qs(&alloc); // sorted queries
    crd::containers::Array<f64> qr(&alloc); // random queries
    x.resize(n);
    y.resize(n);
    qs.resize(nq);
    qr.resize(nq);
    u64 s = 12345;
    auto rnd = [&]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<f64>(s >> 11) / static_cast<f64>(static_cast<u64>(1) << 53);
    };
    f64 acc = 0.0;
    for (usize i = 0; i < n; ++i)
    {
        acc += 0.5 + rnd();
        x[i] = acc;
        y[i] = rnd() * 10.0 - 5.0;
    }
    const f64 lo = x[0];
    const f64 hi = x[n - 1];
    for (usize i = 0; i < nq; ++i)
    {
        qs[i] = lo + (hi - lo) * static_cast<f64>(i) / static_cast<f64>(nq); // sorted (resampling case)
        qr[i] = lo + (hi - lo) * rnd();                                       // random (scattered case)
    }
    const auto xs = ConstSpan<f64>{x.data(), n};
    const auto ys = ConstSpan<f64>{y.data(), n};

    auto bench_build = [&](const char* name, int reps, auto build_fn) {
        build_fn();
        const auto t0 = clk::now();
        for (int r = 0; r < reps; ++r)
        {
            build_fn();
        }
        const auto t1 = clk::now();
        const f64 us = std::chrono::duration<f64, std::micro>(t1 - t0).count() / reps;
        std::printf("%-18s build %8.2f us/fit   %10.0f fits/s\n", name, us, 1e6 / us);
    };
    auto bench_eval = [&](const char* name, const char* qn, ConstSpan<f64> q, auto eval_fn) {
        const auto t0 = clk::now();
        for (usize i = 0; i < nq; ++i)
        {
            acc += eval_fn(q[i]);
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / static_cast<f64>(nq);
        std::printf("%-18s eval-%-6s %6.2f ns/pt   %8.1f Mevals/s\n", name, qn, ns, 1e3 / ns);
    };

    PchipInterpolant<f64> pchip(&alloc);
    CubicSplineInterpolant<f64> nak(&alloc);
    CubicSplineInterpolant<f64> nat(&alloc);
    bench_build("PCHIP", 20000, [&] { return pchip.build(xs, ys); });
    bench_build("CubicSpline-nak", 20000, [&] { return nak.build(xs, ys, SplineBC::NotAKnot); });
    bench_build("CubicSpline-nat", 20000, [&] { return nat.build(xs, ys, SplineBC::Natural); });

    bench_eval("PCHIP", "sorted", ConstSpan<f64>{qs.data(), nq}, [&](f64 q) { return pchip.eval(q); });
    bench_eval("PCHIP", "random", ConstSpan<f64>{qr.data(), nq}, [&](f64 q) { return pchip.eval(q); });
    bench_eval("CubicSpline-nak", "sorted", ConstSpan<f64>{qs.data(), nq}, [&](f64 q) { return nak.eval(q); });
    usize lc = 0;
    bench_eval("linear", "sorted", ConstSpan<f64>{qs.data(), nq},
               [&](f64 q) { return interp_linear(xs, ys, q, lc); });

    // --- v13-c: makima (local, n=1000) ---
    AkimaInterpolant<f64> mak(&alloc);
    bench_build("makima", 20000, [&] { return mak.build(xs, ys, true); });
    bench_eval("makima", "sorted", ConstSpan<f64>{qs.data(), nq}, [&](f64 q) { return mak.eval(q); });

    // --- v13-c: barycentric + Floater-Hormann (global, O(n)/eval ⇒ benched on a moderate n=30) ---
    constexpr usize ng = 30;
    constexpr usize ngq = 10000;
    crd::containers::Array<f64> gx(&alloc);
    crd::containers::Array<f64> gy(&alloc);
    crd::containers::Array<f64> gq(&alloc);
    gx.resize(ng);
    gy.resize(ng);
    gq.resize(ngq);
    f64 ga = 0.0;
    for (usize i = 0; i < ng; ++i)
    {
        ga += 0.5 + rnd();
        gx[i] = ga;
        gy[i] = rnd() * 10.0 - 5.0;
    }
    for (usize i = 0; i < ngq; ++i)
    {
        gq[i] = gx[0] + (gx[ng - 1] - gx[0]) * static_cast<f64>(i) / static_cast<f64>(ngq);
    }
    const auto gxs = ConstSpan<f64>{gx.data(), ng};
    const auto gys = ConstSpan<f64>{gy.data(), ng};
    BarycentricInterpolant<f64> bc(&alloc);
    FloaterHormannInterpolant<f64> fh(&alloc);
    bench_build("barycentric n30", 50000, [&] { return bc.build(gxs, gys); });
    bench_build("FloaterHorm n30", 50000, [&] { return fh.build(gxs, gys, 3); });
    auto bench_eval_g = [&](const char* name, auto eval_fn) {
        const auto t0 = clk::now();
        for (usize i = 0; i < ngq; ++i)
        {
            acc += eval_fn(gq[i]);
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / static_cast<f64>(ngq);
        std::printf("%-18s eval-n30    %6.2f ns/pt\n", name, ns);
    };
    bench_eval_g("barycentric n30", [&](f64 q) { return bc.eval(q); });
    bench_eval_g("FloaterHorm n30", [&](f64 q) { return fh.eval(q); });

    // --- v13-d: Chebyshev (N=64), Padé [4/4], Trig (N=64) ---
    auto bench_eval_n = [&](const char* name, f64 lo, f64 hi, usize npts, auto eval_fn) {
        const auto t0 = clk::now();
        for (usize i = 0; i < npts; ++i)
        {
            acc += eval_fn(lo + (hi - lo) * static_cast<f64>(i) / static_cast<f64>(npts));
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / static_cast<f64>(npts);
        std::printf("%-18s eval        %6.2f ns/pt\n", name, ns);
    };
    constexpr usize ncb = 64;
    crd::containers::Array<f64> chy(&alloc);
    crd::containers::Array<f64> chn(&alloc);
    crd::containers::Array<f64> trgy(&alloc);
    chy.resize(ncb);
    chn.resize(ncb);
    trgy.resize(ncb);
    chebyshev_nodes<f64>(ncb, -1.0, 1.0, crd::containers::Span<f64>{chn.data(), ncb});
    for (usize i = 0; i < ncb; ++i)
    {
        chy[i] = 1.0 / (1.0 + chn[i] * chn[i]);
        trgy[i] = rnd() * 2.0 - 1.0;
    }
    ChebyshevInterpolant<f64> cheb(&alloc);
    TrigInterpolant<f64> trg(&alloc);
    bench_build("Chebyshev n64", 20000, [&] { return cheb.build(ConstSpan<f64>{chy.data(), ncb}, -1.0, 1.0); });
    bench_eval_n("Chebyshev n64", -1.0, 1.0, 20000, [&](f64 q) { return cheb.eval(q); });
    {
        constexpr usize nb = 20000;
        crd::containers::Array<f64> bq(&alloc);
        crd::containers::Array<f64> bo(&alloc);
        crd::containers::Array<f64> bb1(&alloc);
        crd::containers::Array<f64> bb2(&alloc);
        bq.resize(nb);
        bo.resize(nb);
        bb1.resize(nb);
        bb2.resize(nb);
        for (usize i = 0; i < nb; ++i)
        {
            bq[i] = -1.0 + 2.0 * static_cast<f64>(i) / static_cast<f64>(nb);
        }
        const auto t0 = clk::now();
        for (int r = 0; r < 50; ++r)
        {
            cheb.eval_batch(ConstSpan<f64>{bq.data(), nb}, crd::containers::Span<f64>{bo.data(), nb},
                            crd::containers::Span<f64>{bb1.data(), nb}, crd::containers::Span<f64>{bb2.data(), nb});
        }
        const auto t1 = clk::now();
        acc += bo[0];
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / 50.0 / static_cast<f64>(nb);
        std::printf("%-18s eval-batch  %6.2f ns/pt\n", "Chebyshev n64", ns);
    }
    bench_build("Trig n64", 20000, [&] { return trg.build(ConstSpan<f64>{trgy.data(), ncb}, 0.0, 6.2831853); });
    bench_eval_n("Trig n64", 0.0, 6.2831853, 20000, [&](f64 q) { return trg.eval(q); });
    crd::containers::Array<f64> tcf(&alloc);
    tcf.resize(9);
    tcf[0] = 1.0;
    for (usize i = 1; i < 9; ++i)
    {
        tcf[i] = tcf[i - 1] / static_cast<f64>(i);
    }
    RationalPade<f64> pade(&alloc);
    bench_build("Pade 4/4", 50000, [&] { return pade.build(ConstSpan<f64>{tcf.data(), 9}, 4, 4); });
    bench_eval_n("Pade 4/4", 0.0, 2.0, 100000, [&](f64 q) { return pade.eval(q); });

    // --- v13-e: RBF (N=100 scattered 2-D, gaussian) ---
    constexpr usize nrb = 100;
    crd::containers::Array<f64> rp(&alloc);
    crd::containers::Array<f64> rv(&alloc);
    crd::containers::Array<f64> rqx(&alloc);
    rp.resize(nrb * 2);
    rv.resize(nrb);
    rqx.resize(2000);
    for (usize i = 0; i < nrb; ++i)
    {
        rp[2 * i] = rnd();
        rp[2 * i + 1] = rnd();
        rv[i] = rnd() * 2.0 - 1.0;
    }
    for (usize i = 0; i < 2000; ++i)
    {
        rqx[i] = rnd();
    }
    RbfInterpolant<f64> rbf(&alloc);
    bench_build("RBF gauss n100", 3000, [&] {
        return rbf.build(ConstSpan<f64>{rp.data(), nrb * 2}, ConstSpan<f64>{rv.data(), nrb}, nrb, 2,
                         RbfKernel::Gaussian, 1.0, 0);
    });
    rbf.build(ConstSpan<f64>{rp.data(), nrb * 2}, ConstSpan<f64>{rv.data(), nrb}, nrb, 2, RbfKernel::Gaussian, 1.0, 0);
    {
        const auto t0 = clk::now();
        for (int r = 0; r < 10; ++r)
        {
            for (usize i = 0; i < 1000; ++i)
            {
                acc += rbf.eval(ConstSpan<f64>{&rqx[2 * i], 2});
            }
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / 10.0 / 1000.0;
        std::printf("%-18s eval        %6.2f ns/pt\n", "RBF gauss n100", ns);
    }

    // --- v13-f: gridded N-linear (100x100 2-D, 100k queries) ---
    constexpr usize gn = 100;
    crd::containers::Array<f64> gv(&alloc);
    crd::containers::Array<f64> gqx(&alloc);
    gv.resize(gn * gn);
    gqx.resize(200000);
    for (usize i = 0; i < gn * gn; ++i)
    {
        gv[i] = rnd();
    }
    for (usize i = 0; i < 200000; ++i)
    {
        gqx[i] = rnd() * static_cast<f64>(gn - 1);
    }
    constexpr f64 g_origin[2] = {0.0, 0.0};
    constexpr f64 g_spacing[2] = {1.0, 1.0};
    constexpr usize g_count[2] = {gn, gn};
    RegularGridInterpolant<f64> grid(&alloc);
    grid.build(ConstSpan<f64>{g_origin, 2}, ConstSpan<f64>{g_spacing, 2}, ConstSpan<usize>{g_count, 2}, 2,
               ConstSpan<f64>{gv.data(), gn * gn});
    {
        const auto t0 = clk::now();
        for (usize i = 0; i < 100000; ++i)
        {
            acc += grid.eval_linear(ConstSpan<f64>{&gqx[2 * i], 2});
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / 100000.0;
        std::printf("%-18s eval        %6.2f ns/pt\n", "grid-linear 2D", ns);
    }
    {
        const auto t0 = clk::now();
        for (usize i = 0; i < 100000; ++i)
        {
            acc += grid.eval_cubic(ConstSpan<f64>{&gqx[2 * i], 2});
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / 100000.0;
        std::printf("%-18s eval        %6.2f ns/pt\n", "grid-cubic 2D", ns);
    }
    // --- v13-f: 1-D grid cubic (n=1000) — the dim where Boost cardinal_cubic_b_spline applies ---
    {
        constexpr usize g1n = 1000;
        crd::containers::Array<f64> g1v(&alloc);
        crd::containers::Array<f64> q1(&alloc);
        g1v.resize(g1n);
        q1.resize(100000);
        for (usize i = 0; i < g1n; ++i)
        {
            g1v[i] = rnd();
        }
        for (usize i = 0; i < 100000; ++i)
        {
            q1[i] = rnd() * static_cast<f64>(g1n - 1);
        }
        constexpr f64 o1[1] = {0.0};
        constexpr f64 s1[1] = {1.0};
        constexpr usize c1[1] = {g1n};
        RegularGridInterpolant<f64> grid1(&alloc);
        grid1.build(ConstSpan<f64>{o1, 1}, ConstSpan<f64>{s1, 1}, ConstSpan<usize>{c1, 1}, 1,
                    ConstSpan<f64>{g1v.data(), g1n});
        const auto t0 = clk::now();
        for (usize i = 0; i < 100000; ++i)
        {
            acc += grid1.eval_cubic(ConstSpan<f64>{&q1[i], 1});
        }
        const auto t1 = clk::now();
        const f64 ns = std::chrono::duration<f64, std::nano>(t1 - t0).count() / 100000.0;
        std::printf("%-18s eval        %6.2f ns/pt\n", "grid-cubic 1D", ns);
    }

    std::printf("# checksum %.3f\n", acc);
    return 0;
}
