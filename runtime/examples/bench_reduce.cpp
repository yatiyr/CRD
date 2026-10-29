// v14-c reduction bench — Tier-D sum vs numpy, Tier-R binned vs ReproBLAS
// (oracle baselines in external/PEER_ORACLES.md; same sine workload). Both tiers run the hesap-tensor kernel level the
// process selected (CRD_CPU_LEVEL forces one, scripts/run_reduce_bench.sh runs each); each sum must give the bits of
// the portable level's kernels in the same order (Tier D's blocks folded left to right, Tier R's binned deposits; both
// are bit-exact on every level), or the run exits 1.
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/tensor/reduce.hpp>
#include <crd/hesap/tensor/tensor_dispatch.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace crd::hesap::tensor;

namespace
{
template <typename F> double med10(crd::u64 elems, F fn)
{
    volatile crd::f64 sink = 0.0;
    (void)sink;
    double times[10];
    fn(); // warm
    for (int r = 0; r < 10; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        times[r] = std::chrono::duration<double, std::nano>(t1 - t0).count();
    }
    std::sort(times, times + 10);
    return times[4] / static_cast<double>(elems);
}

// Whether got has the bits of want; prints the miss.
bool same_bits(const char* tier, crd::u64 n, crd::f64 got, crd::f64 want)
{
    if (std::bit_cast<crd::u64>(got) == std::bit_cast<crd::u64>(want))
    {
        return true;
    }
    std::printf("!MISMATCH! %s at N=%llu: %a, portable %a\n", tier, static_cast<unsigned long long>(n), got, want);
    return false;
}
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1ULL << 30U);
    const TensorKernels& portable = *tensor_kernel_levels()[0].table;
    bool ok = true;
    std::printf("hesap-tensor kernel level %s (selected %s)\n", crd::cpu_level_name(tensor_kernels().level),
                crd::cpu_level_name(crd::cpu_selection().selected));
    for (const crd::u64 n : {crd::u64{1} << 20U, crd::u64{1} << 24U})
    {
        const crd::u64 shape[] = {n};
        Tensor<crd::f64> t(&alloc, shape);
        for (crd::u64 i = 0; i < n; ++i)
        {
            // the ReproBLAS harness workload: sin(2*pi*(i/N - 0.5))
            t.data()[i] = std::sin(6.283185307179586 * (static_cast<double>(i) / static_cast<double>(n) - 0.5));
        }
        TensorView<const crd::f64> v = t.view();

        crd::f64 tier_d = 0.0;
        for (crd::u64 off = 0; off < n; off += detail::kReduceBlock)
        {
            const crd::u64 len = n - off < detail::kReduceBlock ? n - off : detail::kReduceBlock;
            const crd::f64 partial =
                portable.reduce_f64[static_cast<crd::u32>(ReduceOp::Sum)].block(v.data() + off, len, 0.0);
            tier_d = off == 0U ? partial : tier_d + partial;
        }
        detail::BinnedF64 tier_r;
        portable.binned_sum_f64(tier_r.rungs, v.data(), n);
        ok = same_bits("TierD", n, reduce_sum(v), tier_d) && ok;
        ok = same_bits("TierR", n, reduce_sum_reproducible(v), tier_r.convert()) && ok;

        volatile crd::f64 sink;
        const double d = med10(n, [&] { sink = reduce_sum(v); });
        const double r = med10(n, [&] { sink = reduce_sum_reproducible(v); });
        // naive left-to-right for the contrast row
        const double nv = med10(n,
                                [&]
                                {
                                    crd::f64 s = 0.0;
                                    const crd::f64* p = v.data();
                                    for (crd::u64 i = 0; i < n; ++i)
                                    {
                                        s += p[i];
                                    }
                                    sink = s;
                                });
        std::printf("N=%llu\n", static_cast<unsigned long long>(n));
        std::printf("  naive L2R            %8.4f ns/elem\n", nv);
        std::printf("  TierD fixed-tree     %8.4f ns/elem\n", d);
        std::printf("  TierR binned (ours)  %8.4f ns/elem\n", r);
    }
    return ok ? 0 : 1;
}
