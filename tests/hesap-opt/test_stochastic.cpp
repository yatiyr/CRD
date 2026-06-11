// crd-hesap-opt v7-i — the stochastic/ML optimizer family + the Philox-backed minibatch sampler. Validates:
// (1) the SHARP per-rule gates — the EXACT closed-form first/second steps of each update rule, written out
// from the reference formulas independently of the implementation (catches state-indexing / update-order /
// bias-correction transcription bugs); (2) all ten rules converge on a strongly convex quadratic
// (deterministic, fixed budgets); (3) LR schedules + gradient clipping closed-form; (4) the minibatch
// sampler's BY-CONSTRUCTION reproducibility: epoch-keyed Philox streams ⇒ the same (seed, epoch) gives the
// same permutation regardless of run history or epoch VISIT ORDER (replay can jump to any epoch) + each epoch
// is a true permutation; (5) the determinism moats: full-batch Adam over the parallel-but-bit-exact spmv is
// bit-identical across {1,2,4,8,16} workers, and the full minibatch-SGD pipeline is bit-identical across
// independent runs; (6) boundary n = 0.

#include <crd/hesap/opt/opt.hpp>
#include <crd/hesap/sparse/parallel_sparse_linear_op.hpp>
#include <crd/hesap/sparse/sparse_linear_op.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cmath>

namespace opt = crd::hesap::opt;
namespace sp = crd::hesap::sparse;

namespace
{
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

Csr laplacian_1d(crd::memory::IAllocator* a, crd::u32 n)
{
    sp::TripletBuilder<crd::f64> tb(a, n, n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        tb.add(i, i, 2.0);
        if (i + 1 < n)
        {
            tb.add(i, i + 1, -1.0);
            tb.add(i + 1, i, -1.0);
        }
    }
    return tb.compress();
}
} // namespace

TEST_CASE("v7-i closed-form steps: SGD / momentum / Nesterov", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::f64 g[] = {0.5};

    {
        opt::SgdConfig<crd::f64> cfg;
        cfg.lr = 0.1;
        opt::SgdOptimizer<crd::f64> sgd(&alloc, 1, cfg);
        crd::f64 x[] = {1.0};
        sgd.step({x, 1}, {g, 1});
        CHECK(x[0] == 1.0 - 0.1 * 0.5); // plain: x −= lr·g, exactly
    }
    {
        opt::SgdConfig<crd::f64> cfg;
        cfg.lr = 0.1;
        cfg.momentum = 0.9;
        opt::SgdOptimizer<crd::f64> sgd(&alloc, 1, cfg);
        crd::f64 x[] = {1.0};
        sgd.step({x, 1}, {g, 1}); // torch: first buffer v = g ⇒ x = 1 − 0.1·0.5
        CHECK(x[0] == 1.0 - 0.05);
        sgd.step({x, 1}, {g, 1}); // v = 0.9·0.5 + 0.5 = 0.95 ⇒ x −= 0.095
        CHECK(std::fabs(x[0] - (0.95 - 0.095)) < 1e-15);
    }
    {
        opt::SgdConfig<crd::f64> cfg;
        cfg.lr = 0.1;
        cfg.momentum = 0.9;
        cfg.nesterov = true;
        opt::SgdOptimizer<crd::f64> sgd(&alloc, 1, cfg);
        crd::f64 x[] = {1.0};
        sgd.step({x, 1}, {g, 1}); // v = 0.5; g_eff = 0.5 + 0.9·0.5 = 0.95 ⇒ x = 1 − 0.095
        CHECK(std::fabs(x[0] - 0.905) < 1e-15);
    }
}

TEST_CASE("v7-i closed-form steps: Adam / AdamW / RAdam / Lion", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);

    {
        opt::AdamConfig<crd::f64> cfg; // defaults: lr 1e-3, betas 0.9/0.999, eps 1e-8
        opt::AdamOptimizer<crd::f64> adam(&alloc, 1, cfg);
        crd::f64 x[] = {0.0};
        const crd::f64 g[] = {2.0};
        adam.step({x, 1}, {g, 1});
        // Step 1: m̂ = g, v̂ = g² ⇒ Δx = −lr·g/(|g|+ε), exactly.
        const crd::f64 expect = -cfg.lr * 2.0 / (std::sqrt(4.0) + cfg.eps);
        CHECK(std::fabs(x[0] - expect) < 1e-18);
    }
    {
        opt::AdamConfig<crd::f64> cfg;
        cfg.weight_decay = 0.01;
        cfg.decoupled = true; // AdamW
        opt::AdamOptimizer<crd::f64> adamw(&alloc, 1, cfg);
        crd::f64 x[] = {1.0};
        const crd::f64 g[] = {2.0};
        adamw.step({x, 1}, {g, 1});
        // Decoupled: x ← x − lr·wd·x first, then the pure-Adam step on the UNDECAYED gradient.
        const crd::f64 expect = 1.0 - cfg.lr * 0.01 * 1.0 - cfg.lr * 2.0 / (std::sqrt(4.0) + cfg.eps);
        CHECK(std::fabs(x[0] - expect) < 1e-18);
    }
    {
        opt::RadamConfig<crd::f64> cfg; // β2 = 0.999 ⇒ ρ₁ = ρ∞ − 2·0.999/0.001 = 1999 − 1998 = 1 ≤ 5
        opt::RadamOptimizer<crd::f64> radam(&alloc, 1, cfg);
        crd::f64 x[] = {0.5};
        const crd::f64 g[] = {3.0};
        radam.step({x, 1}, {g, 1});
        CHECK(std::fabs(x[0] - (0.5 - cfg.lr * 3.0)) < 1e-15); // un-rectified branch: x −= lr·m̂ = lr·g
    }
    {
        opt::LionConfig<crd::f64> cfg;
        cfg.lr = 0.01;
        cfg.weight_decay = 0.1;
        opt::LionOptimizer<crd::f64> lion(&alloc, 1, cfg);
        crd::f64 x[] = {1.0};
        const crd::f64 g[] = {-3.0};
        lion.step({x, 1}, {g, 1});
        // u = (1−β1)·g = −0.3 ⇒ sign = −1; x −= lr·(sign + wd·x) = 0.01·(−1 + 0.1) = −0.009.
        CHECK(std::fabs(x[0] - 1.009) < 1e-15);
    }
}

TEST_CASE("v7-i closed-form steps: RMSprop / Adagrad / Adadelta / LAMB / Nadam", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::f64 g[] = {2.0};

    {
        opt::RmspropConfig<crd::f64> cfg; // α = 0.99, lr = 1e-2, eps = 1e-8
        opt::RmspropOptimizer<crd::f64> rms(&alloc, 1, cfg);
        crd::f64 x[] = {0.0};
        rms.step({x, 1}, {g, 1});
        const crd::f64 sq = 0.01 * 4.0; // (1−α)g²
        const crd::f64 expect = -cfg.lr * 2.0 / (std::sqrt(sq) + cfg.eps);
        CHECK(std::fabs(x[0] - expect) < 1e-16);
    }
    {
        opt::AdagradConfig<crd::f64> cfg; // lr = 1e-2, eps = 1e-10
        opt::AdagradOptimizer<crd::f64> ada(&alloc, 1, cfg);
        crd::f64 x[] = {0.0};
        ada.step({x, 1}, {g, 1});
        const crd::f64 expect = -cfg.lr * 2.0 / (std::sqrt(4.0) + cfg.eps);
        CHECK(std::fabs(x[0] - expect) < 1e-16);
    }
    {
        opt::AdadeltaConfig<crd::f64> cfg; // ρ = 0.9, eps = 1e-6, lr = 1
        opt::AdadeltaOptimizer<crd::f64> add(&alloc, 1, cfg);
        crd::f64 x[] = {0.0};
        add.step({x, 1}, {g, 1});
        const crd::f64 sq = 0.1 * 4.0;
        const crd::f64 delta = std::sqrt(cfg.eps) / std::sqrt(sq + cfg.eps) * 2.0;
        CHECK(std::fabs(x[0] - (-cfg.lr * delta)) < 1e-16);
    }
    {
        opt::LambConfig<crd::f64> cfg; // lr 1e-3, eps 1e-6
        opt::LambOptimizer<crd::f64> lamb(&alloc, 2, cfg);
        crd::f64 x[] = {3.0, 4.0}; // ‖x‖ = 5
        const crd::f64 g2[] = {2.0, 0.0};
        lamb.step({x, 2}, {g2, 2});
        // r = (g/( |g|+ε ), 0) ≈ (0.999999..., 0); trust = 5/‖r‖; Δx = −lr·trust·r.
        const crd::f64 r0 = 2.0 / (std::sqrt(4.0) + cfg.eps);
        const crd::f64 trust = 5.0 / r0;
        CHECK(std::fabs(x[0] - (3.0 - cfg.lr * trust * r0)) < 1e-12);
        CHECK(x[1] == 4.0); // zero-gradient lane: r = 0 ⇒ unchanged
    }
    {
        opt::NadamConfig<crd::f64> cfg; // the PyTorch μ-product schedule
        opt::NadamOptimizer<crd::f64> nadam(&alloc, 1, cfg);
        crd::f64 x[] = {0.0};
        nadam.step({x, 1}, {g, 1});
        // Step 1 from the reference formula (μ_t = β1(1 − ½·0.96^{tψ})), written out independently:
        const crd::f64 mu1 = cfg.beta1 * (1.0 - 0.5 * std::pow(0.96, 1.0 * cfg.momentum_decay));
        const crd::f64 mu2 = cfg.beta1 * (1.0 - 0.5 * std::pow(0.96, 2.0 * cfg.momentum_decay));
        const crd::f64 m1 = (1.0 - cfg.beta1) * 2.0;
        const crd::f64 v1 = (1.0 - cfg.beta2) * 4.0;
        const crd::f64 mhat = mu2 * m1 / (1.0 - mu1 * mu2) + (1.0 - mu1) * 2.0 / (1.0 - mu1);
        const crd::f64 vhat = v1 / (1.0 - cfg.beta2);
        const crd::f64 expect = -cfg.lr * mhat / (std::sqrt(vhat) + cfg.eps);
        CHECK(std::fabs(x[0] - expect) < 1e-15);
    }
}

TEST_CASE("v7-i: all ten rules converge on a strongly convex quadratic", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::usize n = 2;
    const crd::f64 d[] = {1.0, 2.0};  // f = ½ Σ d_i (x_i − c_i)²
    const crd::f64 c[] = {1.0, -2.0}; // the minimizer
    auto grad_at = [&](const crd::f64* x, crd::f64* g)
    {
        for (crd::usize i = 0; i < n; ++i)
        {
            g[i] = d[i] * (x[i] - c[i]);
        }
    };
    auto run = [&](auto& optimizer, crd::usize steps) -> crd::f64
    {
        crd::f64 x[] = {0.0, 0.0};
        crd::f64 g[2];
        for (crd::usize t = 0; t < steps; ++t)
        {
            grad_at(x, g);
            optimizer.step({x, n}, {g, n});
        }
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            err = std::max(err, std::fabs(x[i] - c[i]));
        }
        return err;
    };

    {
        opt::SgdConfig<crd::f64> cfg;
        cfg.lr = 0.2;
        opt::SgdOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 200) < 1e-6);
    }
    {
        opt::SgdConfig<crd::f64> cfg;
        cfg.lr = 0.1;
        cfg.momentum = 0.9;
        cfg.nesterov = true;
        opt::SgdOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 400) < 1e-6);
    }
    {
        opt::AdamConfig<crd::f64> cfg;
        cfg.lr = 0.05;
        opt::AdamOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 3000) < 1e-4);
    }
    {
        opt::AdamConfig<crd::f64> cfg;
        cfg.lr = 0.05;
        cfg.weight_decay = 1e-6;
        cfg.decoupled = true;
        opt::AdamOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 3000) < 1e-3); // tiny decoupled decay biases the fixed point ⇒ looser tol
    }
    {
        opt::NadamConfig<crd::f64> cfg;
        cfg.lr = 0.05;
        opt::NadamOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 3000) < 1e-4);
    }
    {
        opt::RadamConfig<crd::f64> cfg;
        cfg.lr = 0.05;
        opt::RadamOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 3000) < 1e-4);
    }
    {
        opt::RmspropConfig<crd::f64> cfg;
        cfg.lr = 0.01;
        opt::RmspropOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 5000) < 1e-4);
    }
    {
        opt::AdagradConfig<crd::f64> cfg;
        cfg.lr = 1.0; // Adagrad's denominator grows ⇒ needs a generous base rate
        opt::AdagradOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 5000) < 1e-3);
    }
    {
        opt::AdadeltaConfig<crd::f64> cfg;
        opt::AdadeltaOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 20000) < 1e-3); // self-scaled and slow by design
    }
    {
        opt::LionConfig<crd::f64> cfg;
        cfg.lr = 0.005; // sign updates orbit the minimizer at the lr scale ⇒ small lr + loose tol
        opt::LionOptimizer<crd::f64> o(&alloc, n, cfg);
        CHECK(run(o, 4000) < 0.02);
    }
    {
        // LAMB's trust-ratio update is NORMALIZED (r ≈ ±1 per lane when gradients dominate ε) ⇒ with a constant
        // lr it orbits the minimizer at the lr·trust scale — in practice LAMB always runs under a decay schedule.
        // Anneal to 0 over the run (also exercises set_lr + lr_cosine_annealing in a real loop).
        opt::LambConfig<crd::f64> cfg;
        cfg.lr = 0.02;
        opt::LambOptimizer<crd::f64> o(&alloc, n, cfg);
        crd::f64 x[] = {0.0, 0.0};
        crd::f64 g[2];
        const crd::usize steps = 4000;
        for (crd::usize t = 0; t < steps; ++t)
        {
            o.set_lr(opt::lr_cosine_annealing<crd::f64>(0.02, t, steps));
            grad_at(x, g);
            o.step({x, n}, {g, n});
        }
        crd::f64 err = 0.0;
        for (crd::usize i = 0; i < n; ++i)
        {
            err = std::max(err, std::fabs(x[i] - c[i]));
        }
        CHECK(err < 1e-3);
    }
}

TEST_CASE("v7-i LR schedules + clipping: closed forms", "[hesap][opt][v7]")
{
    CHECK(opt::lr_step_decay(1.0, 0, 10, 0.5) == 1.0);
    CHECK(opt::lr_step_decay(1.0, 9, 10, 0.5) == 1.0);
    CHECK(opt::lr_step_decay(1.0, 10, 10, 0.5) == 0.5);
    CHECK(opt::lr_step_decay(1.0, 25, 10, 0.5) == 0.25);
    CHECK(opt::lr_exponential(1.0, 3, 0.5) == 0.125);
    CHECK(opt::lr_cosine_annealing(1.0, crd::usize{0}, 100, 0.1) == 1.0);
    CHECK(std::fabs(opt::lr_cosine_annealing(1.0, crd::usize{50}, 100, 0.1) - 0.55) < 1e-12);
    CHECK(std::fabs(opt::lr_cosine_annealing(1.0, crd::usize{100}, 100, 0.1) - 0.1) < 1e-12);
    CHECK(std::fabs(opt::lr_linear_warmup(1.0, 0, 4) - 0.25) < 1e-15);
    CHECK(std::fabs(opt::lr_linear_warmup(1.0, 3, 4) - 1.0) < 1e-15);
    CHECK(opt::lr_linear_warmup(1.0, 100, 4) == 1.0);

    crd::f64 g[] = {3.0, 4.0}; // ‖g‖ = 5
    const crd::f64 pre = opt::clip_grad_norm<crd::f64>({g, 2}, 1.0);
    CHECK(std::fabs(pre - 5.0) < 1e-15);
    CHECK(std::fabs(g[0] - 0.6) < 1e-15);
    CHECK(std::fabs(g[1] - 0.8) < 1e-15);
    const crd::f64 pre2 = opt::clip_grad_norm<crd::f64>({g, 2}, 10.0); // already inside: unchanged
    CHECK(std::fabs(pre2 - 1.0) < 1e-15);
    CHECK(std::fabs(g[0] - 0.6) < 1e-15);

    crd::f64 h[] = {-3.0, 0.5, 2.0};
    opt::clip_grad_value<crd::f64>({h, 3}, 1.0);
    CHECK(h[0] == -1.0);
    CHECK(h[1] == 0.5);
    CHECK(h[2] == 1.0);
}

TEST_CASE("v7-i minibatch sampler: by-construction reproducibility + permutation", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    const crd::usize n = 103; // prime ⇒ a short last batch (the boundary case)
    const crd::usize bs = 16;
    opt::MinibatchSampler s1(&alloc, n, bs, /*seed=*/77);
    opt::MinibatchSampler s2(&alloc, n, bs, 77);

    CHECK(s1.num_batches() == 7); // 6×16 + 7

    // Epoch visited IN ORDER on s1; OUT OF ORDER on s2 — epoch-keyed streams must agree regardless.
    s1.begin_epoch(0);
    crd::containers::Array<crd::u32> e0(&alloc);
    e0.resize(n);
    {
        crd::usize w = 0;
        for (crd::usize k = 0; k < s1.num_batches(); ++k)
        {
            const auto b = s1.batch(k);
            for (crd::usize i = 0; i < b.size(); ++i)
            {
                e0[w++] = b[i];
            }
        }
        CHECK(w == n);
    }
    s1.begin_epoch(1);
    s1.begin_epoch(2);

    s2.begin_epoch(2); // jump straight to epoch 2 — replay semantics
    crd::containers::Array<bool> seen(&alloc);
    seen.resize(n);
    for (crd::usize i = 0; i < n; ++i)
    {
        seen[i] = false;
    }
    bool identical = true;
    {
        crd::usize w = 0;
        for (crd::usize k = 0; k < s2.num_batches(); ++k)
        {
            const auto b = s2.batch(k);
            for (crd::usize i = 0; i < b.size(); ++i)
            {
                REQUIRE(b[i] < n);
                REQUIRE_FALSE(seen[b[i]]); // a true permutation across the epoch's batches
                seen[b[i]] = true;
                ++w;
            }
        }
        CHECK(w == n);
    }
    // s1 (visited 0,1,2 in order) and s2 (jumped to 2): identical epoch-2 batches.
    s1.begin_epoch(2);
    s2.begin_epoch(2);
    for (crd::usize k = 0; k < s1.num_batches(); ++k)
    {
        const auto a = s1.batch(k);
        const auto b = s2.batch(k);
        REQUIRE(a.size() == b.size());
        for (crd::usize i = 0; i < a.size(); ++i)
        {
            identical = identical && (a[i] == b[i]);
        }
    }
    CHECK(identical);

    // Different epochs differ (overwhelmingly).
    s1.begin_epoch(3);
    bool differs = false;
    {
        crd::usize w = 0;
        for (crd::usize k = 0; k < s1.num_batches() && !differs; ++k)
        {
            const auto b = s1.batch(k);
            for (crd::usize i = 0; i < b.size(); ++i, ++w)
            {
                differs = differs || (b[i] != e0[w]);
            }
        }
    }
    CHECK(differs);
}

TEST_CASE("v7-i moat: full-batch Adam over the parallel spmv is bit-identical {1,2,4,8,16}", "[hesap][opt][v7][moat]")
{
    const crd::u32 n = 64;
    crd::memory::TlsfAllocator alloc(1U << 25);
    Csr a = laplacian_1d(&alloc, n);
    sp::SparseLinearOp<crd::f64> serial_op(a);

    crd::containers::Array<crd::f64> xtrue(&alloc);
    crd::containers::Array<crd::f64> b(&alloc);
    xtrue.resize(n);
    b.resize(n);
    for (crd::u32 i = 0; i < n; ++i)
    {
        xtrue[i] = 1.0 + 0.02 * static_cast<crd::f64>(i);
    }
    (void)serial_op.apply({xtrue.data(), n}, {b.data(), n});

    crd::containers::Array<crd::f64> x_ref(&alloc);
    bool have_ref = false;
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        {
            sp::ParallelSparseLinearOp<crd::f64> op(a, &alloc, /*parallel_min_stored_bytes=*/0);
            opt::QuadraticObjective<crd::f64> obj(op, {b.data(), n}, &alloc); // ∇f = parallel spmv − b
            opt::AdamConfig<crd::f64> acfg;
            acfg.lr = 0.05;
            opt::AdamOptimizer<crd::f64> adam(&alloc, n, acfg);

            crd::containers::Array<crd::f64> x(&alloc);
            crd::containers::Array<crd::f64> g(&alloc);
            x.resize(n);
            g.resize(n);
            for (crd::u32 i = 0; i < n; ++i)
            {
                x[i] = 0.0;
            }
            for (int t = 0; t < 50; ++t)
            {
                (void)obj.gradient({x.data(), n}, {g.data(), n});
                adam.step({x.data(), n}, {g.data(), n});
            }
            if (!have_ref)
            {
                x_ref.resize(n);
                for (crd::u32 i = 0; i < n; ++i)
                {
                    x_ref[i] = x[i];
                }
                have_ref = true;
            }
            else
            {
                bool ident = true;
                for (crd::u32 i = 0; i < n && ident; ++i)
                {
                    ident = (x[i] == x_ref[i]);
                }
                CHECK(ident); // the training trajectory rides the parallel-but-bit-exact spmv
            }
        }
        crd::jobs::shutdown();
    }
}

TEST_CASE("v7-i moat: the minibatch-SGD pipeline reproduces bit-identically across runs", "[hesap][opt][v7][moat]")
{
    crd::memory::TlsfAllocator alloc(1U << 22);
    // Scalar linear regression y = w·z over N samples; the per-batch gradient is (1/|B|)Σ (w·z_i − y_i)·z_i.
    const crd::usize num = 64;
    crd::containers::Array<crd::f64> z(&alloc);
    crd::containers::Array<crd::f64> y(&alloc);
    z.resize(num);
    y.resize(num);
    const crd::f64 w_true = 1.7;
    for (crd::usize i = 0; i < num; ++i)
    {
        z[i] = 0.1 * static_cast<crd::f64>(i % 13) - 0.6;
        y[i] = w_true * z[i] + (i % 2 == 0 ? 0.01 : -0.01); // deterministic "noise"
    }

    auto train = [&](crd::f64& w_out)
    {
        opt::MinibatchSampler sampler(&alloc, num, 8, /*seed=*/2024);
        opt::SgdConfig<crd::f64> cfg;
        cfg.lr = 0.1;
        cfg.momentum = 0.9;
        opt::SgdOptimizer<crd::f64> sgd(&alloc, 1, cfg);
        crd::f64 w = 0.0;
        for (crd::u64 epoch = 0; epoch < 10; ++epoch)
        {
            // The constant-lr SGD noise ball shrinks under the standard per-epoch decay (lr ∝ 1/(1+e)).
            sgd.set_lr(0.1 / static_cast<crd::f64>(1 + epoch));
            sampler.begin_epoch(epoch);
            for (crd::usize k = 0; k < sampler.num_batches(); ++k)
            {
                const auto batch = sampler.batch(k);
                crd::f64 g = 0.0;
                for (crd::usize i = 0; i < batch.size(); ++i)
                {
                    const crd::usize s = batch[i];
                    g += (w * z[s] - y[s]) * z[s];
                }
                g /= static_cast<crd::f64>(batch.size());
                sgd.step({&w, 1}, {&g, 1});
            }
        }
        w_out = w;
    };

    crd::f64 w1 = 0.0;
    crd::f64 w2 = 0.0;
    train(w1);
    train(w2);
    CHECK(w1 == w2);                      // bit-identical end-to-end (sampler + stepper)
    CHECK(std::fabs(w1 - w_true) < 0.05); // and it actually learns the slope
}

TEST_CASE("v7-i boundary: n = 0 steps are no-ops", "[hesap][opt][v7]")
{
    crd::memory::TlsfAllocator alloc(1U << 20);
    crd::containers::Span<crd::f64> empty{static_cast<crd::f64*>(nullptr), 0};
    crd::containers::ConstSpan<crd::f64> cempty{static_cast<const crd::f64*>(nullptr), 0};

    opt::SgdOptimizer<crd::f64> sgd(&alloc, 0, opt::SgdConfig<crd::f64>{});
    sgd.step(empty, cempty);
    opt::AdamOptimizer<crd::f64> adam(&alloc, 0, opt::AdamConfig<crd::f64>{});
    adam.step(empty, cempty);
    opt::LambOptimizer<crd::f64> lamb(&alloc, 0, opt::LambConfig<crd::f64>{});
    lamb.step(empty, cempty);
    CHECK(sgd.steps() == 1);
    CHECK(adam.steps() == 1);
    CHECK(lamb.steps() == 1);
}
