// test_ckir_nrc.cpp — D-007 B14-d: the Neural Radiance Cache primitives (ckir_nrc.hpp) on the CPU oracle. B14-d-1 verifies the
// MULTIRESOLUTION HASH-GRID ENCODER (Instant-NGP): (1) a zero feature table encodes to zero; (2) TRILINEAR PARTITION OF UNITY —
// if every table feature equals a constant C, every encoded feature equals C (the 8 corner weights sum to 1, independent of
// hash collisions) — the exactness anchor; (3) determinism (same position ⇒ same encoding). Portability rides test_vulkan.

#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_nrc.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace kir = crd::kir;

namespace
{
crd::usize uz(int v) { return static_cast<crd::usize>(v); }
} // namespace

TEST_CASE("B14-d hash-grid encoder: zero table, trilinear partition-of-unity, determinism", "[kir][nrc]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::nrc::NrcConfig        cfg;
    const int                  lf  = cfg.encoded_dim();       // L·F
    const int                  ts  = cfg.levels * cfg.table_size * cfg.features;
    const int                  n   = 256;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::nrc::build_nrc_hashgrid_encode(g, cfg);

    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> tab(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    pos.resize(uz(n * 3));
    tab.resize(uz(ts));
    out.resize(uz(n * lf));
    crd::u32 s   = 12345U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n * 3; ++i) { pos[uz(i)] = rnd(); } // positions in [0,1)³

    const auto run = [&]() {
        kir::KernelBuffer b[3] = {{pos.data(), n * 3, 0, 0}, {tab.data(), ts, 0, 1}, {out.data(), n * lf, 0, 2}};
        kir::eval_cpu_kernel(g, e, b, 3, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
    };

    // (1) a ZERO feature table ⇒ zero encoding everywhere.
    for (int i = 0; i < ts; ++i) { tab[uz(i)] = 0.0; }
    run();
    double mx = 0.0;
    for (int i = 0; i < n * lf; ++i) { mx = std::fabs(out[uz(i)]) > mx ? std::fabs(out[uz(i)]) : mx; }
    CHECK(mx == 0.0);

    // (2) PARTITION OF UNITY: a CONSTANT table (all features = C) ⇒ every encoded feature == C (the 8 trilinear weights sum to
    // 1, regardless of which entries the corners hash to). The single strongest correctness check for a trilinear encoder.
    const double c = 0.375;
    for (int i = 0; i < ts; ++i) { tab[uz(i)] = c; }
    run();
    double maxerr = 0.0;
    for (int i = 0; i < n * lf; ++i) { maxerr = std::fabs(out[uz(i)] - c) > maxerr ? std::fabs(out[uz(i)] - c) : maxerr; }
    CHECK(maxerr < 1e-6); // trilinear weights partition unity ⇒ Σ w·C = C (f32 sum ULP only)

    // (3) DETERMINISM: a varied (index-hashed) table, run twice ⇒ byte-identical.
    for (int i = 0; i < ts; ++i) { tab[uz(i)] = static_cast<double>((i * 2654435761ULL >> 15) & 255ULL) / 255.0; }
    run();
    crd::containers::Array<crd::f64> first(&alloc);
    first.resize(uz(n * lf));
    for (int i = 0; i < n * lf; ++i) { first[uz(i)] = out[uz(i)]; }
    run();
    double ddiff = 0.0;
    for (int i = 0; i < n * lf; ++i) { ddiff = std::fabs(out[uz(i)] - first[uz(i)]) > ddiff ? std::fabs(out[uz(i)] - first[uz(i)]) : ddiff; }
    CHECK(ddiff == 0.0);

    // the varied table also produces a genuinely NON-constant encoding (the features actually vary across positions).
    double lo = 1e30;
    double hi = -1e30;
    for (int i = 0; i < n * lf; ++i) { lo = out[uz(i)] < lo ? out[uz(i)] : lo; hi = out[uz(i)] > hi ? out[uz(i)] : hi; }
    CHECK(hi - lo > 0.1);
}

TEST_CASE("B14-d MLP inference: one ReLU hidden layer == a hand-computed forward pass", "[kir][nrc]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::nrc::NrcConfig        cfg;
    const int                  d = cfg.encoded_dim();
    const int                  h = cfg.hidden;
    const int                  o = cfg.out_dim;
    const int                  n = 128;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::nrc::build_nrc_infer(g, cfg);

    crd::containers::Array<crd::f64> enc(&alloc);
    crd::containers::Array<crd::f64> w1(&alloc);
    crd::containers::Array<crd::f64> b1(&alloc);
    crd::containers::Array<crd::f64> w2(&alloc);
    crd::containers::Array<crd::f64> b2(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    enc.resize(uz(n * d));
    w1.resize(uz(h * d));
    b1.resize(uz(h));
    w2.resize(uz(o * h));
    b2.resize(uz(o));
    out.resize(uz(n * o));
    crd::u32 s   = 999U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(static_cast<float>(static_cast<double>(s >> 8) / static_cast<double>(1U << 24) * 2.0 - 1.0)); };
    for (int i = 0; i < n * d; ++i) { enc[uz(i)] = rnd(); }
    for (int i = 0; i < h * d; ++i) { w1[uz(i)] = rnd() * 0.5; }
    for (int i = 0; i < h; ++i) { b1[uz(i)] = rnd() * 0.1; }
    for (int i = 0; i < o * h; ++i) { w2[uz(i)] = rnd() * 0.5; }
    for (int i = 0; i < o; ++i) { b2[uz(i)] = rnd() * 0.1; }

    kir::KernelBuffer bufs[6] = {{enc.data(), n * d, 0, 0}, {w1.data(), h * d, 0, 1}, {b1.data(), h, 0, 2},
                                 {w2.data(), o * h, 0, 3}, {b2.data(), o, 0, 4}, {out.data(), n * o, 0, 5}};
    kir::eval_cpu_kernel(g, e, bufs, 6, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    // hand-computed reference forward (f32-rounded op-by-op to match the oracle's per-op rounding).
    const auto f32 = [](double v) { return static_cast<double>(static_cast<float>(v)); };
    double     maxerr = 0.0;
    for (int p = 0; p < n; ++p)
    {
        double hh[64];
        for (int j = 0; j < h; ++j)
        {
            double acc = b1[uz(j)];
            for (int k = 0; k < d; ++k) { acc = f32(acc + f32(f32(w1[uz(j * d + k)]) * f32(enc[uz(p * d + k)]))); }
            hh[j] = acc > 0.0 ? acc : 0.0;
        }
        for (int c = 0; c < o; ++c)
        {
            double acc = b2[uz(c)];
            for (int j = 0; j < h; ++j) { acc = f32(acc + f32(f32(w2[uz(c * h + j)]) * f32(hh[j]))); }
            maxerr = std::fabs(acc - out[uz(p * o + c)]) > maxerr ? std::fabs(acc - out[uz(p * o + c)]) : maxerr;
        }
    }
    CHECK(maxerr < 1e-5); // per-op-f32 reference matches the oracle's forward exactly (only accumulation-order-consistent adds)
}

TEST_CASE("B14-d training backprop: the analytic weight gradient == finite differences of the L2 loss", "[kir][nrc]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::nrc::NrcConfig        cfg;
    const int                  d = cfg.encoded_dim();
    const int                  h = cfg.hidden;
    const int                  o = cfg.out_dim;
    const int                  n = 64;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::nrc::build_nrc_train_grad(g, cfg);

    crd::containers::Array<crd::f64> enc(&alloc);
    crd::containers::Array<crd::f64> w1(&alloc);
    crd::containers::Array<crd::f64> b1(&alloc);
    crd::containers::Array<crd::f64> w2(&alloc);
    crd::containers::Array<crd::f64> b2(&alloc);
    crd::containers::Array<crd::f64> tgt(&alloc);
    crd::containers::Array<crd::f64> gw1(&alloc);
    crd::containers::Array<crd::f64> gw2(&alloc);
    enc.resize(uz(n * d));
    w1.resize(uz(h * d));
    b1.resize(uz(h));
    w2.resize(uz(o * h));
    b2.resize(uz(o));
    tgt.resize(uz(n * o));
    gw1.resize(uz(n * h * d));
    gw2.resize(uz(n * o * h));
    crd::u32 s   = 321U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24) * 2.0 - 1.0; };
    for (int i = 0; i < n * d; ++i) { enc[uz(i)] = rnd(); }
    for (int i = 0; i < h * d; ++i) { w1[uz(i)] = rnd() * 0.5; }
    for (int i = 0; i < h; ++i) { b1[uz(i)] = rnd() * 0.2; } // biased so some pre-activations are negative (ReLU mask exercised)
    for (int i = 0; i < o * h; ++i) { w2[uz(i)] = rnd() * 0.5; }
    for (int i = 0; i < o; ++i) { b2[uz(i)] = rnd() * 0.1; }
    for (int i = 0; i < n * o; ++i) { tgt[uz(i)] = rnd(); }

    kir::KernelBuffer bufs[8] = {{enc.data(), n * d, 0, 0}, {w1.data(), h * d, 0, 1}, {b1.data(), h, 0, 2}, {w2.data(), o * h, 0, 3},
                                 {b2.data(), o, 0, 4}, {tgt.data(), n * o, 0, 5}, {gw1.data(), n * h * d, 0, 6}, {gw2.data(), n * o * h, 0, 7}};
    kir::eval_cpu_kernel(g, e, bufs, 8, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    // f64 loss for ONE sample p (forward): L = Σ_o (out_o − tgt_o)², with W2[oo,jj] optionally perturbed.
    const auto loss = [&](int p, int pw2, double dw2, int pw1, double dw1) {
        double hh[64];
        for (int j = 0; j < h; ++j)
        {
            double acc = b1[uz(j)];
            for (int k = 0; k < d; ++k) { double ww = w1[uz(j * d + k)] + (j * d + k == pw1 ? dw1 : 0.0); acc += ww * enc[uz(p * d + k)]; }
            hh[j] = acc > 0.0 ? acc : 0.0;
        }
        double l = 0.0;
        for (int c = 0; c < o; ++c)
        {
            double acc = b2[uz(c)];
            for (int j = 0; j < h; ++j) { double ww = w2[uz(c * h + j)] + (c * h + j == pw2 ? dw2 : 0.0); acc += ww * hh[j]; }
            l += (acc - tgt[uz(p * o + c)]) * (acc - tgt[uz(p * o + c)]);
        }
        return l;
    };

    const double eps = 1e-4;
    int          p   = 7;      // a representative sample
    double       maxrel = 0.0;
    // check several W2 entries (dense grad) and several W1 entries (ReLU-masked grad) via central differences.
    for (int idx : {0, 5, 17, 31, 47})
    {
        const double num = (loss(p, idx, eps, -1, 0.0) - loss(p, idx, -eps, -1, 0.0)) / (2.0 * eps);
        const double ana = gw2[uz(p * o * h + idx)];
        maxrel = std::max(maxrel, std::fabs(num - ana) / (std::fabs(num) + 1e-3));
    }
    for (int idx : {0, 11, 40, 77, 100})
    {
        const double num = (loss(p, -1, 0.0, idx, eps) - loss(p, -1, 0.0, idx, -eps)) / (2.0 * eps);
        const double ana = gw1[uz(p * h * d + idx)];
        maxrel = std::max(maxrel, std::fabs(num - ana) / (std::fabs(num) + 1e-3));
    }
    CHECK(maxrel < 5e-3); // analytic backprop == numerical gradient (central-difference accuracy in f32)

    // the gradients are genuinely non-trivial (not all zero — the network is actually learning signal).
    double gmax = 0.0;
    for (int i = 0; i < n * o * h; ++i) { gmax = std::fabs(gw2[uz(i)]) > gmax ? std::fabs(gw2[uz(i)]) : gmax; }
    CHECK(gmax > 0.05);
}
