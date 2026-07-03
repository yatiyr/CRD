// v14-c reduction bench — Tier-D sum vs numpy, Tier-R binned vs ReproBLAS
// (oracle baselines in external/PEER_ORACLES.md; same sine workload).
#include <crd/hesap/tensor/reduce.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
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
} // namespace

int main()
{
    crd::memory::TlsfAllocator alloc(1ULL << 30U);
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
    return 0;
}
