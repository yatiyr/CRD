// test_determinism_moat.cpp -- Phase 3.1.6 v16-i: the DETERMINISTIC-TRAINING MOAT. `batch_gradient` folds per-sample
// gradients in a FIXED sample order (never an atomic scatter-add), so the batched gradient is BIT-IDENTICAL across
// {1..16} workers -- and therefore a whole TRAINING RUN (SGD over that gradient) replays bit-for-bit, run-to-run AND
// worker-count-invariant. PyTorch/JAX cannot: their reduction order is nondeterministic. This is the certifiable-
// learned-controller moat (replay a training run bit-for-bit). ADR-0097.

#include <crd/hesap/autodiff/reverse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace rev = crd::hesap::autodiff::reverse;
using crd::f64;

namespace
{
// per-sample loss for a tiny linear controller: loss_s = (theta . x_s - y_s)^2 ; batch_gradient sums Sigma_s.
struct RegLoss
{
    const f64* x; // S*d row-major features
    const f64* y; // S targets
    int        d;
    rev::Var   operator()(const rev::Var* theta, int, int s) const
    {
        rev::Var acc = theta[0] * x[s * d + 0];
        for (int j = 1; j < d; ++j) { acc = acc + theta[j] * x[s * d + j]; }
        const rev::Var r = acc - y[s];
        return r * r;
    }
};
} // namespace

TEST_CASE("v16-i: batch_gradient {1..16}-worker BIT-IDENTICAL + a training run replays bit-for-bit (the moat)",
          "[autodiff][reverse][moat]")
{
    crd::jobs::init(crd::jobs::Config{16U});
    {
        constexpr int d     = 4;
        constexpr int ssamp = 40;
        f64           x[ssamp * d];
        f64           yv[ssamp];
        for (int s = 0; s < ssamp; ++s)
        {
            for (int j = 0; j < d; ++j) { x[s * d + j] = std::sin(0.3 + s + 2.0 * j); }
            yv[s] = 0.4 + 0.2 * std::cos(0.7 + s);
        }
        const RegLoss loss{x, yv, d};

        // 16 per-worker tapes (each its own allocator -- concurrent forward, no shared adjoints)
        crd::memory::TlsfAllocator a00(1 << 20);
        crd::memory::TlsfAllocator a01(1 << 20);
        crd::memory::TlsfAllocator a02(1 << 20);
        crd::memory::TlsfAllocator a03(1 << 20);
        crd::memory::TlsfAllocator a04(1 << 20);
        crd::memory::TlsfAllocator a05(1 << 20);
        crd::memory::TlsfAllocator a06(1 << 20);
        crd::memory::TlsfAllocator a07(1 << 20);
        crd::memory::TlsfAllocator a08(1 << 20);
        crd::memory::TlsfAllocator a09(1 << 20);
        crd::memory::TlsfAllocator a10(1 << 20);
        crd::memory::TlsfAllocator a11(1 << 20);
        crd::memory::TlsfAllocator a12(1 << 20);
        crd::memory::TlsfAllocator a13(1 << 20);
        crd::memory::TlsfAllocator a14(1 << 20);
        crd::memory::TlsfAllocator a15(1 << 20);
        rev::Tape                  t00(&a00);
        rev::Tape                  t01(&a01);
        rev::Tape                  t02(&a02);
        rev::Tape                  t03(&a03);
        rev::Tape                  t04(&a04);
        rev::Tape                  t05(&a05);
        rev::Tape                  t06(&a06);
        rev::Tape                  t07(&a07);
        rev::Tape                  t08(&a08);
        rev::Tape                  t09(&a09);
        rev::Tape                  t10(&a10);
        rev::Tape                  t11(&a11);
        rev::Tape                  t12(&a12);
        rev::Tape                  t13(&a13);
        rev::Tape                  t14(&a14);
        rev::Tape                  t15(&a15);
        rev::Tape* tapes[16] = {&t00, &t01, &t02, &t03, &t04, &t05, &t06, &t07,
                                &t08, &t09, &t10, &t11, &t12, &t13, &t14, &t15};
        f64 gbuf[ssamp * d];

        // (1) the batched gradient is BIT-IDENTICAL for every worker count 1..16
        f64 theta0[d] = {0.5, -0.3, 0.8, 0.1};
        f64 gref[d];
        rev::batch_gradient(loss, {theta0, d}, ssamp, {gref, d}, {tapes, crd::usize{1}}, {gbuf, ssamp * d}, 1U);
        for (crd::u32 nj = 2; nj <= 16U; ++nj)
        {
            f64 g[d];
            rev::batch_gradient(loss, {theta0, d}, ssamp, {g, d}, {tapes, static_cast<crd::usize>(nj)}, {gbuf, ssamp * d}, nj);
            for (int i = 0; i < d; ++i) { CHECK(g[i] == gref[i]); } // exact bit-identity
        }

        // (2) a full SGD training run replays bit-for-bit -- run-to-run AND worker-count-invariant
        const auto run_sgd = [&](crd::u32 nj, f64* theta_out)
        {
            f64 theta[d] = {0.0, 0.0, 0.0, 0.0};
            f64 g[d];
            for (int epoch = 0; epoch < 60; ++epoch)
            {
                rev::batch_gradient(loss, {theta, d}, ssamp, {g, d}, {tapes, static_cast<crd::usize>(nj)}, {gbuf, ssamp * d},
                                    nj);
                for (int i = 0; i < d; ++i) { theta[i] -= 0.02 * g[i] / static_cast<f64>(ssamp); }
            }
            for (int i = 0; i < d; ++i) { theta_out[i] = theta[i]; }
        };
        f64 w_run_a[d];
        f64 w_run_b[d];
        f64 w_16[d];
        run_sgd(1U, w_run_a); // run 1, single worker
        run_sgd(1U, w_run_b); // run 2, single worker
        run_sgd(16U, w_16);   // run 3, sixteen workers
        for (int i = 0; i < d; ++i)
        {
            CHECK(w_run_a[i] == w_run_b[i]); // run-to-run bit-identical
            CHECK(w_run_a[i] == w_16[i]);    // worker-count-invariant bit-identical
        }
    }
    crd::jobs::shutdown();
}
