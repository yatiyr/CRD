// cufft_2d_c2c_batched_bench.cu — B16-a-2 FFT-ocean crush baseline: the VENDOR BATCHED 2-D complex-to-complex INVERSE FFT.
// This is the exact peer for our build_fft2d_c2c_batched (the ocean transforms its 4 packed complex fields — height/
// displacement/slope/Jacobian — in ONE batched inverse 2-D FFT; with C cascades the batch is 4·C). cuFFT does a batched 2-D
// C2C inverse via cufftPlanMany + cufftExecC2C(CUFFT_INVERSE). Both sides are UNNORMALISED (the ocean folds no 1/N²). The
// crush lever is the same as the whole campaign: once the batch·image working set spills L2 (Ada ~32-48 MB), the transform is
// DRAM-BOUND and our 2-pass transpose-on-write strided IFFT (row IFFT + strided column IFFT = fewer global round-trips) beats
// cuFFT's transpose-based multipass. Below the L2 spill it is compute/L2-bound and bit-exactness (no FMA) costs us — parity.
// Compare per-batch ms to the Vulkan [.ocean-ifft-bench] number (last_gpu_ms, same n/batch).
// Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh.

#include "cuda_bench.hpp"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

namespace
{
constexpr double kMebibyte = 1024.0 * 1024.0;

void sweep(int n)
{
    const int       batches[] = {4, 8, 16, 32, 64};             // 4 packed ocean fields × {1,2,4,8,16} cascades
    const long long rc        = static_cast<long long>(n) * n; // one image (complex elements)
    std::printf("# cuFFT BATCHED 2-D C2C INVERSE, %dx%d, whole-batch time (batched inverse 2-D FFT), image = %.2f MB\n",
                n, n, static_cast<double>(rc * static_cast<long long>(sizeof(cufftComplex))) / kMebibyte);
    std::printf("# %-6s %-14s %-16s %-12s\n", "B", "batch_ms", "per_image_ms", "workset_MB");
    for (const int b : batches)
    {
        const long long tot = rc * b;
        cufftComplex*   d   = nullptr;
        CRD_CUDA_CHECK(cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(tot)));
        CRD_CUDA_CHECK(cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(tot)));

        cufftHandle plan;
        int         dims[2] = {n, n};
        CRD_CUDA_CHECK(cufftPlanMany(&plan, 2, dims, nullptr, 1, static_cast<int>(rc), nullptr, 1, static_cast<int>(rc),
                                     CUFFT_C2C, b));
        const auto inverse = [&]
        {
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_INVERSE));
        };
        for (int i = 0; i < 5; ++i) // warmup
        {
            inverse();
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const float  best    = crd_bench::best_event_ms(30, inverse); // kernel-only: the events bracket the transform
        const double workset = static_cast<double>(tot * static_cast<long long>(sizeof(cufftComplex))) / kMebibyte;
        std::printf("%-6d %-14.4f %-16.5f %-12.2f\n", b, static_cast<double>(best), static_cast<double>(best) / b,
                    workset);
        CRD_CUDA_CHECK(cufftDestroy(plan));
        CRD_CUDA_CHECK(cudaFree(d));
    }
    std::printf("\n");
}
} // namespace

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# device: %s, L2 %.1f MB, bus %d-bit\n", prop.name, static_cast<double>(prop.l2CacheSize) / kMebibyte,
                prop.memoryBusWidth);
    sweep(256);
    sweep(512);
    return 0;
}
