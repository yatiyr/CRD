// v12-e RNG throughput — Cerid bulk-fill ns/u64 + GB/s (vs NumPy random_raw, timed by bench_rng_refs.py). Counter
// and small-state engines; single-thread, in-cache-ish bulk. Plain crd containers (no STL).

#include <crd/hesap/stats/stats.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

namespace st = crd::hesap::stats;
using crd::u64;
using crd::usize;

namespace
{
template <class Gen, class FillFn>
void row_impl(const char* name, Gen make, FillFn fill)
{
    crd::memory::TlsfAllocator alloc(static_cast<usize>(1) << 27);
    const usize n = static_cast<usize>(1) << 22; // 4M u64
    crd::containers::Array<u64> buf(&alloc);
    buf.resize(n);
    auto g = make();
    fill(g, crd::containers::Span<u64>(buf.data(), n)); // warm
    const int reps = 20;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < reps; ++r)
    {
        fill(g, crd::containers::Span<u64>(buf.data(), n));
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(reps) * n);
    std::printf("%-14s %6.3f ns/u64   %6.2f GB/s\n", name, ns, 8.0 / ns);
}

template <class Gen>
void row(const char* name, Gen make) // scalar generic fill
{
    row_impl(name, make, [](auto& g, crd::containers::Span<u64> s) { st::fill_u64(g, s); });
}

template <class Gen>
void row_simd(const char* name, Gen make) // AVX2 member fill (counter engines)
{
    row_impl(name, make, [](auto& g, crd::containers::Span<u64> s) { g.fill(s); });
}
} // namespace

int main()
{
    std::printf("# Cerid RNG bulk throughput (single-thread, ns per u64)\n");
    row("xoshiro256**", [] { return st::Xoshiro256ss(12345); });
    row("xoshiro256++", [] { return st::Xoshiro256pp(12345); });
    row("sfc64", [] { return st::Sfc64(12345); });
    row("pcg64-dxsm", [] { return st::Pcg64Dxsm(12345); });
    row("splitmix64", [] { return st::SplitMix64(12345); });
    row("mt19937", [] { return st::Mt19937(12345U); });
    row_simd("threefry4x64", [] { return st::ThreefryRng(12345); }); // AVX2 4-block
    row_simd("philox4x32", [] { return st::PhiloxRng(12345); });     // AVX2 8-block
    return 0;
}
