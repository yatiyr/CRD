// v14-g hyper-optimizer gates - increment A: HyperNet cost model + T=0 greedy
// (cotengra-faithful; python oracle boards in
// docs/bench/2026-07-05-v14g-hyperopt-oracle.md).
#include <crd/hesap/tensor/hyperopt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::hesap::tensor::HyperGreedyOptions;
using crd::hesap::tensor::HyperObjective;
using crd::hesap::tensor::HyperStatus;
using crd::hesap::tensor::HyperTree;
using crd::hesap::tensor::hyper_greedy;
using crd::hesap::tensor::hyperdetail::HyperNet;

namespace
{

// ids helper: build ConstSpan-of-ConstSpan from C arrays
template <crd::u32 N>
struct TermList
{
    crd::containers::ConstSpan<crd::u32> spans[N];
    [[nodiscard]] crd::containers::ConstSpan<crd::containers::ConstSpan<crd::u32>> all() const { return {spans, N}; }
};

} // namespace

TEST_CASE("hyperopt: matmul-chain net - cost model + T=0 greedy pick the cheap order", "[v14g][hyperopt]")
{
    // ab,bc,cd->ad with a=32 b=16 c=64 d=8 (indices 0=a 1=b 2=c 3=d).
    // Greedy T=0 must contract bc,cd first (score 128-1536) then ab,bd:
    // flops = 16*64*8 + 32*16*8 = 8192 + 4096 = 12288 (cotengra metric).
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U, 2U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 out[] = {0U, 3U};
    const crd::u64 sizes[] = {32U, 16U, 64U, 8U};
    TermList<3> terms{{{t0, 2}, {t1, 2}, {t2, 2}}};

    crd::memory::TlsfAllocator alloc(1U << 20);
    HyperNet net(&alloc);
    REQUIRE(net.build(terms.all(), {out, 2}, {sizes, 4}) == HyperStatus::Ok);
    REQUIRE(net.n_leaves() == 3U);
    REQUIRE(net.n_alive() == 3U);

    crd::hesap::stats::PhiloxRng rng(0U, 0U);
    HyperGreedyOptions opts; // T=0 deterministic
    REQUIRE(hyper_greedy(net, opts, rng, 0.0) == HyperStatus::Ok);
    REQUIRE(net.n_alive() == 1U);
    REQUIRE(net.total_flops() == 12288.0);
    // the first recorded pair must be (1, 2) = bc x cd
    REQUIRE(net.ssa_a().size() == 2U);
    REQUIRE(net.ssa_a()[0] == 1U);
    REQUIRE(net.ssa_b()[0] == 2U);
}

TEST_CASE("hyperopt: size-1 indices are skipped; disconnected leftovers contract by size", "[v14g][hyperopt]")
{
    // Two disconnected pairs: ab,b + cd,d (b,d contracted away), then the two
    // disconnected results (sizes 4 and 2) merge in the by-size finisher.
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 t3[] = {3U};
    const crd::u64 sizes[] = {4U, 3U, 2U, 5U};
    TermList<4> terms{{{t0, 2}, {t1, 1}, {t2, 2}, {t3, 1}}};

    crd::memory::TlsfAllocator alloc(1U << 20);
    HyperNet net(&alloc);
    REQUIRE(net.build(terms.all(), crd::containers::ConstSpan<crd::u32>{}, {sizes, 4}) == HyperStatus::Ok);
    crd::hesap::stats::PhiloxRng rng(0U, 0U);
    HyperGreedyOptions opts;
    REQUIRE(hyper_greedy(net, opts, rng, 0.0) == HyperStatus::Ok);
    REQUIRE(net.n_alive() == 1U);
    // ab*b = 4*3 = 12 flops; cd*d = 2*5 = 10; final outer 4*2 = 8 → 30 total
    REQUIRE(net.total_flops() == 30.0);
}

TEST_CASE("hyperopt: T>0 greedy is run-twice bit-identical under a fixed seed", "[v14g][hyperopt][determinism]")
{
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U, 2U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 t3[] = {3U, 4U};
    const crd::u32 t4[] = {4U, 0U};
    const crd::u64 sizes[] = {16U, 24U, 32U, 8U, 12U};
    TermList<5> terms{{{t0, 2}, {t1, 2}, {t2, 2}, {t3, 2}, {t4, 2}}};

    crd::u32 a1[8];
    crd::u32 b1[8];
    crd::u32 n1 = 0;
    crd::f64 f1 = 0.0;
    for (int round = 0; round < 2; ++round)
    {
        crd::memory::TlsfAllocator alloc(1U << 20);
        HyperNet net(&alloc);
        REQUIRE(net.build(terms.all(), crd::containers::ConstSpan<crd::u32>{}, {sizes, 5}) == HyperStatus::Ok);
        crd::hesap::stats::PhiloxRng rng(1407U, 3U);
        HyperGreedyOptions opts;
        opts.temperature = 0.5;
        opts.costmod = 1.7;
        REQUIRE(hyper_greedy(net, opts, rng, 0.0) == HyperStatus::Ok);
        if (round == 0)
        {
            n1 = static_cast<crd::u32>(net.ssa_a().size());
            f1 = net.total_flops();
            for (crd::u32 k = 0; k < n1; ++k)
            {
                a1[k] = net.ssa_a()[k];
                b1[k] = net.ssa_b()[k];
            }
        }
        else
        {
            REQUIRE(net.ssa_a().size() == n1);
            REQUIRE(net.total_flops() == f1);
            for (crd::u32 k = 0; k < n1; ++k)
            {
                REQUIRE(net.ssa_a()[k] == a1[k]);
                REQUIRE(net.ssa_b()[k] == b1[k]);
            }
        }
    }
}

TEST_CASE("hyperopt: tree stats replay the contracted net exactly", "[v14g][hyperopt][tree]")
{
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U, 2U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 out[] = {0U, 3U};
    const crd::u64 sizes[] = {32U, 16U, 64U, 8U};
    TermList<3> terms{{{t0, 2}, {t1, 2}, {t2, 2}}};

    crd::memory::TlsfAllocator alloc(1U << 20);
    HyperNet net(&alloc);
    REQUIRE(net.build(terms.all(), {out, 2}, {sizes, 4}) == HyperStatus::Ok);
    crd::hesap::stats::PhiloxRng rng(0U, 0U);
    HyperGreedyOptions opts;
    REQUIRE(hyper_greedy(net, opts, rng, 0.0) == HyperStatus::Ok);

    HyperTree tree(&alloc);
    REQUIRE(tree.build(net) == HyperStatus::Ok);
    const auto st = tree.stats();
    REQUIRE(st.flops == net.total_flops()); // 12288
    REQUIRE(st.size == 256U);               // bd intermediate 16*8... vs ad output 32*8=256
}

TEST_CASE("hyperopt: reconfigure repairs a deliberately bad order to the DP optimum", "[v14g][hyperopt][reconf]")
{
    // ab,bc,cd->ad contracted LEFT-TO-RIGHT (bad): ab*bc = 32768 then ac*cd =
    // 16384 -> 49152 total. The 3-leaf reconfigure must find 12288.
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U, 2U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 out[] = {0U, 3U};
    const crd::u64 sizes[] = {32U, 16U, 64U, 8U};
    TermList<3> terms{{{t0, 2}, {t1, 2}, {t2, 2}}};

    crd::memory::TlsfAllocator alloc(1U << 20);
    HyperNet net(&alloc);
    REQUIRE(net.build(terms.all(), {out, 2}, {sizes, 4}) == HyperStatus::Ok);
    REQUIRE(net.contract(0U, 1U) != HyperNet::kDead); // forced bad order
    REQUIRE(net.contract(3U, 2U) != HyperNet::kDead);
    REQUIRE(net.total_flops() == 49152.0);

    HyperTree tree(&alloc);
    REQUIRE(tree.build(net) == HyperStatus::Ok);
    REQUIRE(tree.stats().flops == 49152.0);
    REQUIRE(tree.reconfigure(8U, 64U, HyperObjective::Flops) == HyperStatus::Ok);
    const auto st = tree.stats();
    REQUIRE(st.flops == 12288.0);
    // run-twice determinism: a second reconfigure changes nothing
    REQUIRE(tree.reconfigure(8U, 64U, HyperObjective::Flops) == HyperStatus::Ok);
    REQUIRE(tree.stats().flops == 12288.0);
}

TEST_CASE("hyperopt: treesa anneal repairs the bad chain and is seed-deterministic", "[v14g][hyperopt][sa]")
{
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U, 2U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 out[] = {0U, 3U};
    const crd::u64 sizes[] = {32U, 16U, 64U, 8U};
    TermList<3> terms{{{t0, 2}, {t1, 2}, {t2, 2}}};

    crd::f64 first_flops = -1.0;
    for (int round = 0; round < 2; ++round)
    {
        crd::memory::TlsfAllocator alloc(1U << 20);
        HyperNet net(&alloc);
        REQUIRE(net.build(terms.all(), {out, 2}, {sizes, 4}) == HyperStatus::Ok);
        REQUIRE(net.contract(0U, 1U) != HyperNet::kDead); // bad order: 49152
        REQUIRE(net.contract(3U, 2U) != HyperNet::kDead);

        HyperTree tree(&alloc);
        REQUIRE(tree.build(net) == HyperStatus::Ok);
        REQUIRE(tree.anneal(2.0, 0.05, 20U, 20U, HyperObjective::Flops, 64.0, 1407U, 5U) == HyperStatus::Ok);
        const auto st = tree.stats();
        // SA with this budget must find the optimum on a 3-leaf tree
        REQUIRE(st.flops == 12288.0);
        if (round == 0)
        {
            first_flops = st.flops;
        }
        else
        {
            REQUIRE(st.flops == first_flops); // seed-deterministic
        }
    }
}

TEST_CASE("hyperopt: slicer honors the memory bound EXACTLY and finds the free slicing", "[v14g][hyperopt][slice]")
{
    using crd::hesap::tensor::HyperSliceResult;
    using crd::hesap::tensor::hyper_slice;
    const crd::u32 t0[] = {0U, 1U};
    const crd::u32 t1[] = {1U, 2U};
    const crd::u32 t2[] = {2U, 3U};
    const crd::u32 out[] = {0U, 3U};
    const crd::u64 sizes[] = {32U, 16U, 64U, 8U};
    TermList<3> terms{{{t0, 2}, {t1, 2}, {t2, 2}}};

    crd::memory::TlsfAllocator alloc(1U << 20);
    HyperNet net(&alloc);
    REQUIRE(net.build(terms.all(), {out, 2}, {sizes, 4}) == HyperStatus::Ok);
    crd::hesap::stats::PhiloxRng rng(0U, 0U);
    HyperGreedyOptions gopts;
    REQUIRE(hyper_greedy(net, gopts, rng, 0.0) == HyperStatus::Ok); // 12288, max size 256

    HyperTree tree(&alloc);
    REQUIRE(tree.build(net) == HyperStatus::Ok);

    HyperSliceResult r1(&alloc);
    REQUIRE(hyper_slice(tree, net, 128U, 7U, 16U, 0.01, r1) == HyperStatus::Ok);
    REQUIRE(r1.max_size <= 128U);          // the EXACT bound
    REQUIRE(r1.sliced_flops == 12288.0);   // free slicing exists on this net
    REQUIRE(r1.overhead == 1.0);
    REQUIRE(r1.indices.size() >= 1U);

    // run-twice bit identity under the same seed
    HyperSliceResult r2(&alloc);
    REQUIRE(hyper_slice(tree, net, 128U, 7U, 16U, 0.01, r2) == HyperStatus::Ok);
    REQUIRE(r2.nslices == r1.nslices);
    REQUIRE(r2.sliced_flops == r1.sliced_flops);
    REQUIRE(r2.max_size == r1.max_size);
    REQUIRE(r2.indices.size() == r1.indices.size());

    // the degenerate bound: slicing EVERY index drives all intermediates to 1
    // element - reachable, and the bound still holds exactly
    HyperSliceResult r3(&alloc);
    REQUIRE(hyper_slice(tree, net, 1U, 7U, 4U, 0.01, r3) == HyperStatus::Ok);
    REQUIRE(r3.max_size == 1U);
}

TEST_CASE("hyperopt: labels-divide contracts a 12-ring fully and is seed-deterministic", "[v14g][hyperopt][labels]")
{
    using crd::hesap::tensor::HyperLabelsOptions;
    using crd::hesap::tensor::hyper_labels_divide;
    // a 12-node ring: term k = {k, (k+1) mod 12}, all dims 4, closed network
    crd::u32 termsbuf[12][2];
    crd::containers::ConstSpan<crd::u32> spans[12];
    crd::u64 sizes[12];
    for (crd::u32 k = 0; k < 12U; ++k)
    {
        termsbuf[k][0] = k;
        termsbuf[k][1] = (k + 1U) % 12U;
        spans[k] = {termsbuf[k], 2};
        sizes[k] = 4U;
    }
    crd::f64 f1 = -1.0;
    for (int round = 0; round < 2; ++round)
    {
        crd::memory::TlsfAllocator alloc(1U << 22);
        HyperNet net(&alloc);
        REQUIRE(net.build({spans, 12}, crd::containers::ConstSpan<crd::u32>{}, {sizes, 12}) == HyperStatus::Ok);
        HyperLabelsOptions opt;
        opt.parts = 4;
        opt.cutoff = 4;
        REQUIRE(hyper_labels_divide(net, opt, 1407U, 9U) == HyperStatus::Ok);
        REQUIRE(net.n_alive() == 1U);
        REQUIRE(net.total_flops() > 0.0);
        if (round == 0)
        {
            f1 = net.total_flops();
        }
        else
        {
            REQUIRE(net.total_flops() == f1); // seed-deterministic
        }
    }
}

TEST_CASE("hyperopt: the driver beats-or-matches plain greedy and is run-twice identical", "[v14g][hyperopt][driver]")
{
    using crd::hesap::tensor::HyperOptOptions;
    using crd::hesap::tensor::HyperOptResult;
    using crd::hesap::tensor::hyper_optimize;
    // 12-ring, dims 4 (as the labels gate)
    crd::u32 termsbuf[12][2];
    crd::containers::ConstSpan<crd::u32> spans[12];
    crd::u64 sizes[12];
    for (crd::u32 k = 0; k < 12U; ++k)
    {
        termsbuf[k][0] = k;
        termsbuf[k][1] = (k + 1U) % 12U;
        spans[k] = {termsbuf[k], 2};
        sizes[k] = 4U;
    }
    crd::memory::TlsfAllocator alloc(1U << 24);
    // baseline: plain T=0 greedy
    HyperNet gnet(&alloc);
    REQUIRE(gnet.build({spans, 12}, crd::containers::ConstSpan<crd::u32>{}, {sizes, 12}) == HyperStatus::Ok);
    crd::hesap::stats::PhiloxRng grng(0U, 0U);
    HyperGreedyOptions gopts;
    REQUIRE(hyper_greedy(gnet, gopts, grng, 0.0) == HyperStatus::Ok);
    const crd::f64 greedy_flops = gnet.total_flops();

    HyperOptOptions opts;
    opts.ntrials = 16;
    opts.seed = 3;
    opts.parallel = false; // serial here; the moat test drives worker counts
    HyperOptResult r1(&alloc);
    REQUIRE(hyper_optimize({spans, 12}, crd::containers::ConstSpan<crd::u32>{}, {sizes, 12}, opts, &alloc, r1) ==
            HyperStatus::Ok);
    REQUIRE(r1.plan.steps.size() == 11U);
    REQUIRE(r1.stats.flops <= greedy_flops); // never worse than one greedy descent
    HyperOptResult r2(&alloc);
    REQUIRE(hyper_optimize({spans, 12}, crd::containers::ConstSpan<crd::u32>{}, {sizes, 12}, opts, &alloc, r2) ==
            HyperStatus::Ok);
    REQUIRE(r2.stats.flops == r1.stats.flops);
    REQUIRE(r2.winner_trial == r1.winner_trial);
    for (crd::usize k = 0; k < r1.plan.steps.size(); ++k)
    {
        REQUIRE(r2.plan.steps[k].a == r1.plan.steps[k].a);
        REQUIRE(r2.plan.steps[k].b == r1.plan.steps[k].b);
    }
}

TEST_CASE("hyperopt: the {1,2,4,8,16} moat - bit-identical plans at every worker count", "[v14g][hyperopt][moat]")
{
    using crd::hesap::tensor::HyperOptOptions;
    using crd::hesap::tensor::HyperOptResult;
    using crd::hesap::tensor::hyper_optimize;
    crd::u32 termsbuf[12][2];
    crd::containers::ConstSpan<crd::u32> spans[12];
    crd::u64 sizes[12];
    for (crd::u32 k = 0; k < 12U; ++k)
    {
        termsbuf[k][0] = k;
        termsbuf[k][1] = (k + 1U) % 12U;
        spans[k] = {termsbuf[k], 2};
        sizes[k] = 4U;
    }
    crd::memory::TlsfAllocator alloc(1U << 24);
    HyperOptOptions opts;
    opts.ntrials = 16;
    opts.seed = 1407;
    opts.parallel = false; // serial reference
    HyperOptResult ref(&alloc);
    REQUIRE(hyper_optimize({spans, 12}, crd::containers::ConstSpan<crd::u32>{}, {sizes, 12}, opts, &alloc, ref) ==
            HyperStatus::Ok);
    for (crd::u32 nw : {1U, 2U, 4U, 8U, 16U})
    {
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        HyperOptOptions popts = opts;
        popts.parallel = true;
        HyperOptResult r(&alloc);
        const HyperStatus st =
            hyper_optimize({spans, 12}, crd::containers::ConstSpan<crd::u32>{}, {sizes, 12}, popts, &alloc, r);
        crd::jobs::shutdown();
        INFO("workers " << nw);
        REQUIRE(st == HyperStatus::Ok);
        REQUIRE(r.stats.flops == ref.stats.flops);
        REQUIRE(r.winner_trial == ref.winner_trial);
        REQUIRE(r.plan.steps.size() == ref.plan.steps.size());
        for (crd::usize k = 0; k < ref.plan.steps.size(); ++k)
        {
            REQUIRE(r.plan.steps[k].a == ref.plan.steps[k].a);
            REQUIRE(r.plan.steps[k].b == ref.plan.steps[k].b);
        }
    }
}
