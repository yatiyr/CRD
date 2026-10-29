// v14-d permute bench — the HPTT head-to-head (docs/bench harness).
// Mirrors external/hptt/crd_hptt_bench.cpp cases exactly (their column-major
// cases mapped to row-major: {1,0} and full-reversal are self-symmetric;
// col-major 512^3 {2,0,1} == row-major {1,2,0}). GiB/s basis = 8 bytes/elem
// (read + write, no write-allocate term) to match the oracle table.
#include <crd/hesap/tensor/permute.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>

using namespace crd::hesap::tensor;

namespace
{
void run_case(crd::memory::IAllocator* alloc, const char* name, crd::containers::ConstSpan<crd::u64> shape,
              crd::containers::ConstSpan<crd::u32> order)
{
    Tensor<crd::f32> src(alloc, shape);
    for (crd::u64 i = 0; i < src.size(); ++i)
    {
        src.data()[i] = static_cast<crd::f32>(i & 0xFFFFU) * 0.001F;
    }
    Tensor<crd::f32> dst(alloc);
    (void)permute_copy(TensorView<const crd::f32>(src.view()), order, dst); // warm + alloc
    (void)permute_copy(TensorView<const crd::f32>(src.view()), order, dst);
    (void)permute_copy(TensorView<const crd::f32>(src.view()), order, dst);

    double times[10];
    for (int r = 0; r < 10; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        (void)permute_copy(TensorView<const crd::f32>(src.view()), order, dst);
        const auto t1 = std::chrono::steady_clock::now();
        times[r] = std::chrono::duration<double, std::nano>(t1 - t0).count();
    }
    std::sort(times, times + 10);
    const double med = times[4];
    const double ns_elem = med / static_cast<double>(src.size());
    const double gibs = (8.0 * static_cast<double>(src.size())) / med; // ns → GB/s ≈ GiB/s basis of the oracle
    std::printf("%-24s %8.4f ns/elem  %7.2f GB/s\n", name, ns_elem, gibs);
}
} // namespace

int main(int argc, char** argv)
{
    const bool mt = argc > 1 && argv[1][0] == char(109); // "mt" / "mt16"
    if (mt)
    {
        const crd::u32 nw = argv[1][2] == char(49) ? 16U : 8U;
        crd::jobs::Config cfg;
        cfg.num_threads = nw;
        crd::jobs::init(cfg);
        std::printf("== MT (%u workers) ==\n", nw);
    }
    crd::memory::TlsfAllocator alloc(3ULL << 30U);
    {
        const crd::u64 s[] = {4096U, 4096U};
        const crd::u32 p[] = {1U, 0U};
        run_case(&alloc, "2D 4096^2 {1,0}", s, p);
    }
    {
        const crd::u64 s[] = {64U, 64U, 64U, 64U};
        const crd::u32 p[] = {3U, 2U, 1U, 0U};
        run_case(&alloc, "4D 64^4 {3,2,1,0}", s, p);
    }
    {
        const crd::u64 s[] = {512U, 512U, 512U};
        const crd::u32 p[] = {1U, 2U, 0U};
        run_case(&alloc, "3D 512^3 {1,2,0}", s, p);
    }
    if (mt)
    {
        crd::jobs::shutdown();
    }
    return 0;
}
