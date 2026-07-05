// v14-g wall-clock bench: hyper_optimize (the full driver, 64 trials) and the
// raw random-greedy engine (x32 trials) on the frozen benchmark corpus.
// Peers timed by scripts/v14g_time_peers.py (cotengra hyper + cotengrust Rust
// random-greedy) on the SAME networks with matched budgets. Board convention:
// docs/bench/README.md; results -> docs/bench/2026-07-05-v14g-hyperopt-oracle.md.
#include <crd/hesap/tensor/hyperopt.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>

#include "../../tests/hesap-tensor/ref_hyperopt.inc"

using crd::hesap::tensor::HyperGreedyOptions;
using crd::hesap::tensor::HyperOptOptions;
using crd::hesap::tensor::HyperOptResult;
using crd::hesap::tensor::HyperStatus;
using crd::hesap::tensor::hyper_greedy;
using crd::hesap::tensor::hyper_optimize;
using crd::hesap::tensor::hyperdetail::HyperNet;

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
};

#define CRD_HY_NET(c)                                                                                              \
    CorpusNet                                                                                                      \
    {                                                                                                              \
        #c, kHy_##c##_n_terms, kHy_##c##_n_indices, kHy_##c##_sizes, kHy_##c##_term_off, kHy_##c##_term_ids,       \
            kHy_##c##_out_ids, kHy_##c##_n_out                                                                     \
    }

const CorpusNet kNets[] = {
    CRD_HY_NET(rand30), CRD_HY_NET(rand60),  CRD_HY_NET(rand120),
    CRD_HY_NET(rand200), CRD_HY_NET(lat8x8), CRD_HY_NET(lat4x4x4),
};

double now_ms()
{
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count()) *
           1e-6;
}

} // namespace

int main()
{
    for (const CorpusNet& net : kNets)
    {
        crd::memory::TlsfAllocator alloc(1U << 28);
        crd::containers::Array<crd::containers::ConstSpan<crd::u32>> terms(&alloc);
        (void)terms.try_reserve(net.n_terms);
        for (crd::u32 t = 0; t < net.n_terms; ++t)
        {
            const crd::u32 b = net.term_off[t];
            (void)terms.try_push_back({net.term_ids + b, net.term_off[t + 1U] - b});
        }
        const crd::containers::ConstSpan<crd::containers::ConstSpan<crd::u32>> ts{terms.data(), terms.size()};
        const crd::containers::ConstSpan<crd::u32> os{net.out_ids, net.n_out};
        const crd::containers::ConstSpan<crd::u64> zs{net.sizes, net.n_indices};

        // --- the full driver, 64 trials + reconf + SA finalists (serial) ---
        HyperOptOptions opts;
        opts.ntrials = 64;
        opts.seed = 3;
        opts.sa_finalists = 3;
        opts.parallel = false;
        HyperOptResult r(&alloc);
        double t0 = now_ms();
        const HyperStatus st = hyper_optimize(ts, os, zs, opts, &alloc, r);
        double t1 = now_ms();
        const double l10 = std::log10(r.stats.flops > 1.0 ? r.stats.flops : 1.0);
        std::printf("[%-9s] driver64+reconf+sa: %8.2f ms  log10=%8.4f  ok=%d\n", net.tag, t1 - t0,
                    l10, st == HyperStatus::Ok ? 1 : 0);

        // --- raw random-greedy x32 (the cotengrust-comparable engine) ---
        HyperNet net0(&alloc);
        (void)net0.build(ts, os, zs);
        double best = 1e300;
        t0 = now_ms();
        for (crd::u32 trial = 0; trial < 32U; ++trial)
        {
            crd::hesap::stats::PhiloxRng prng(7U, 2ULL * trial);
            crd::hesap::stats::PhiloxRng grng(7U, 2ULL * trial + 1ULL);
            HyperGreedyOptions g;
            g.costmod = 0.1 + (4.0 - 0.1) * prng.next_f64();
            g.temperature = std::exp(std::log(0.001) + (std::log(1.0) - std::log(0.001)) * prng.next_f64());
            HyperNet work(&alloc);
            (void)work.clone_from(net0);
            if (hyper_greedy(work, g, grng, best) == HyperStatus::Ok && work.total_flops() < best)
            {
                best = work.total_flops();
            }
        }
        t1 = now_ms();
        std::printf("[%-9s] random-greedy x32 : %8.2f ms  log10=%8.4f\n", net.tag, t1 - t0,
                    std::log10(best > 1.0 ? best : 1.0));
    }
    return 0;
}
