// v14-g corpus quality gates: hyper_optimize at the SAME 64-trial budget must
// land log10(flops) AT OR UNDER cotengra's frozen hq-default results
// (greedy+kahypar, seeded, per-trial reconf - measured 2026-07-05 by
// scripts/v14g_export_corpus.py; the python-oracle boards live in
// docs/bench/2026-07-05-v14g-hyperopt-oracle.md). A miss here is an OPEN bug
// (SANITY #9), not a tolerance.
#include <crd/hesap/tensor/hyperopt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "ref_hyperopt.inc"

using crd::hesap::tensor::HyperOptOptions;
using crd::hesap::tensor::HyperOptResult;
using crd::hesap::tensor::HyperStatus;
using crd::hesap::tensor::hyper_optimize;

namespace
{

struct CorpusNet
{
    const char* tag;
    crd::u32 n_terms;
    crd::u32 n_indices;
    const crd::u64* sizes;
    const crd::u32* term_off;
    const crd::u32* term_ids;
    const crd::u32* out_ids;
    crd::u32 n_out;
    double greedy_log10;
    double ctg_log10;
};

#define CRD_HY_NET(c)                                                                                              \
    CorpusNet                                                                                                      \
    {                                                                                                              \
        #c, kHy_##c##_n_terms, kHy_##c##_n_indices, kHy_##c##_sizes, kHy_##c##_term_off, kHy_##c##_term_ids,       \
            kHy_##c##_out_ids, kHy_##c##_n_out, kHy_##c##_greedy_log10, kHy_##c##_ctg_log10                        \
    }

const CorpusNet kNets[] = {
    CRD_HY_NET(rand30), CRD_HY_NET(rand60),  CRD_HY_NET(rand120),
    CRD_HY_NET(rand200), CRD_HY_NET(lat8x8), CRD_HY_NET(lat4x4x4),
};

} // namespace

TEST_CASE("hyperopt corpus: quality at-or-under cotengra greedy+kahypar @64 trials", "[v14g][hyperopt][corpus]")
{
    for (const CorpusNet& net : kNets)
    {
        crd::memory::TlsfAllocator alloc(1U << 26);
        // spans over the corpus arrays
        crd::containers::Array<crd::containers::ConstSpan<crd::u32>> terms(&alloc);
        REQUIRE(terms.try_reserve(net.n_terms));
        for (crd::u32 t = 0; t < net.n_terms; ++t)
        {
            const crd::u32 b = net.term_off[t];
            const crd::u32 e = net.term_off[t + 1U];
            REQUIRE(terms.try_push_back({net.term_ids + b, e - b}));
        }
        HyperOptOptions opts;
        opts.ntrials = 64;
        opts.seed = 3;
        opts.sa_finalists = 3;
        opts.parallel = false; // serial in the gate; the moat test covers threading
        HyperOptResult r(&alloc);
        const HyperStatus st = hyper_optimize({terms.data(), terms.size()}, {net.out_ids, net.n_out},
                                              {net.sizes, net.n_indices}, opts, &alloc, r);
        INFO(net.tag);
        REQUIRE(st == HyperStatus::Ok);
        const double l10 = std::log10(r.stats.flops > 1.0 ? r.stats.flops : 1.0);
        INFO("ours log10=" << l10 << " ctg-hq=" << net.ctg_log10 << " their-greedy=" << net.greedy_log10);
        CHECK(l10 <= net.ctg_log10); // the crush gate: at-or-under their best default
        REQUIRE(l10 <= net.greedy_log10); // and always at-or-under their greedy floor
    }
}
