// test_ckir_restir.cpp — D-007 B14-a: the ReSTIR reservoir/RIS core (ckir_restir.hpp) on the CPU oracle. The two properties
// that MAKE ReSTIR: (1) RIS is UNBIASED — the mean of the estimate f(y)·W over many pixels equals the true light integral,
// even with an IMPERFECT target p̂; (2) TEMPORAL REUSE reduces variance — merging reservoirs across frames grows the
// effective sample count, so the estimator variance drops while staying unbiased. The candidate generation (which light,
// its contribution + visibility) is the ray-tracing leaf; the estimator math is verified here. Portability in test_vulkan.

#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_restir.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }

// mean + variance of the RIS estimate f(y)·W over pixels; `res` is N·4 reservoirs [f, phat, W, M].
void stats(const crd::containers::Array<crd::f64>& res, int n, double& mean, double& var)
{
    mean = 0.0;
    for (int p = 0; p < n; ++p) { mean += res[uz(p * 4 + 0)] * res[uz(p * 4 + 2)]; }
    mean /= n;
    var = 0.0;
    for (int p = 0; p < n; ++p) { const double est = res[uz(p * 4 + 0)] * res[uz(p * 4 + 2)]; var += (est - mean) * (est - mean); }
    var /= n;
}
} // namespace

TEST_CASE("ReSTIR RIS: the estimate is UNBIASED — mean over pixels == the true light integral (imperfect target)", "[kir][restir]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::restir::RestirConfig  cfg;
    const int                  m = cfg.num_candidates;
    const int                  n = 4096;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::restir::build_restir_ris(g, cfg);

    // 16 lights with contributions f_j; the true integral we estimate = mean_j(f_j). Candidates are drawn UNIFORMLY over
    // lights with an IMPERFECT (constant) target p̂ ⇒ RIS ≈ uniform sampling — the honest unbiasedness test (a perfect p̂=f
    // would be trivially exact). w_i = p̂_i is stored per candidate (source pdf folded in by the caller = constant here).
    const int    lights = 16;
    double       fj[16];
    double       truth = 0.0;
    for (int j = 0; j < lights; ++j) { fj[j] = 0.1 + 0.12 * static_cast<double>(j); truth += fj[j]; }
    truth /= lights;

    crd::containers::Array<crd::f64> cand(&alloc);
    crd::containers::Array<crd::f64> res(&alloc);
    cand.resize(uz(n * m * 3));
    res.resize(uz(n * 4));
    crd::u32 s   = 4242U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < n; ++p)
    {
        for (int i = 0; i < m; ++i)
        {
            const int    j    = static_cast<int>(rnd() * lights) % lights; // uniform light draw
            const int    base = (p * m + i) * 3;
            cand[uz(base + 0)] = fj[j]; // f_i (contribution)
            cand[uz(base + 1)] = 1.0;   // p̂_i = constant ⇒ imperfect target
            cand[uz(base + 2)] = rnd(); // WRS random
        }
    }
    kir::KernelBuffer bufs[2] = {{cand.data(), n * m * 3, 0, 0}, {res.data(), n * 4, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    double mean = 0.0;
    double var  = 0.0;
    stats(res, n, mean, var);
    CHECK(std::abs(mean - truth) < 0.03); // unbiased: the pixel-mean of f(y)·W is the true integral (MC error over 4096 px)
    CHECK(var > 0.0);                     // with a constant target, the single-frame estimator genuinely has variance
}

TEST_CASE("ReSTIR temporal reuse: merging reservoirs across frames DROPS variance (unbiased, more effective samples)", "[kir][restir]")
{
    crd::memory::TlsfAllocator alloc(128U << 20U);
    kir::restir::RestirConfig  cfg;
    const int                  m = cfg.num_candidates;
    const int                  n = 4096;

    kir::KGraph       gr(&alloc);
    const kir::KEntry eris = kir::restir::build_restir_ris(gr, cfg);
    kir::KGraph       gt(&alloc);
    const kir::KEntry etmp = kir::restir::build_restir_temporal(gt, cfg);

    const int lights = 16;
    double    fj[16];
    double    truth = 0.0;
    for (int j = 0; j < lights; ++j) { fj[j] = 0.1 + 0.12 * static_cast<double>(j); truth += fj[j]; }
    truth /= lights;

    crd::containers::Array<crd::f64> cand(&alloc);
    crd::containers::Array<crd::f64> cur(&alloc);
    crd::containers::Array<crd::f64> prev(&alloc);
    crd::containers::Array<crd::f64> xi(&alloc);
    crd::containers::Array<crd::f64> merged(&alloc);
    cand.resize(uz(n * m * 3));
    cur.resize(uz(n * 4));
    prev.resize(uz(n * 4));
    xi.resize(uz(n));
    merged.resize(uz(n * 4));
    crd::u32 s   = 909U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    // The CANONICAL ReSTIR target: p̂ = f (the resampling target equals the integrand — in practice the UNSHADOWED
    // contribution, which the shadow ray then confirms). With a target that tracks the contribution, resampling concentrates
    // the kept sample on high-f lights, and temporally merging the reservoirs averages over the ACCUMULATED candidate stream:
    // the estimate becomes the running mean of all candidates ever seen ⇒ variance ~ Var(f)/M_eff drops as M_eff grows.
    // (Contrast the unbiasedness test above, which deliberately uses the useless p̂=1 to prove RIS is unbiased regardless.)
    const auto gen = [&]() {
        for (int p = 0; p < n; ++p)
        {
            for (int i = 0; i < m; ++i)
            {
                const int j        = static_cast<int>(rnd() * lights) % lights;
                const int base     = (p * m + i) * 3;
                cand[uz(base + 0)] = fj[j];
                cand[uz(base + 1)] = fj[j]; // p̂ = f (canonical target)
                cand[uz(base + 2)] = rnd();
            }
        }
    };
    const auto run_ris = [&](crd::containers::Array<crd::f64>& out) {
        kir::KernelBuffer b[2] = {{cand.data(), n * m * 3, 0, 0}, {out.data(), n * 4, 0, 1}};
        kir::eval_cpu_kernel(gr, eris, b, 2, eris.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
    };

    // frame 0: RIS only ⇒ the single-frame variance
    gen();
    run_ris(prev);
    double mean0 = 0.0;
    double var0  = 0.0;
    stats(prev, n, mean0, var0);

    // frames 1..T: fresh RIS each frame, temporally merged into the running reservoir
    for (int f = 0; f < 12; ++f)
    {
        gen();
        run_ris(cur);
        for (int p = 0; p < n; ++p) { xi[uz(p)] = rnd(); }
        kir::KernelBuffer b[4] = {{cur.data(), n * 4, 0, 0}, {prev.data(), n * 4, 0, 1}, {xi.data(), n, 0, 2}, {merged.data(), n * 4, 0, 3}};
        kir::eval_cpu_kernel(gt, etmp, b, 4, etmp.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
        for (int i = 0; i < n * 4; ++i) { prev[uz(i)] = merged[uz(i)]; }
    }
    double mean_t = 0.0;
    double var_t  = 0.0;
    stats(prev, n, mean_t, var_t);

    CHECK(std::abs(mean_t - truth) < 0.03); // STILL unbiased after temporal reuse
    CHECK(var_t < var0 * 0.5);              // the merged estimator has ≥2× lower variance (more effective samples)
    CHECK(prev[uz(3)] > cur[uz(3)]);       // the merged reservoir's M grew beyond a single frame's M (bounded by the cap)
}
