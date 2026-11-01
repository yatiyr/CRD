// cufft_bench.cu — GPU FFT crush campaign (D-007 B-cmp): the cuFFT GOLD-STANDARD peer harness. Batched C2C forward FFT,
// min-of-N runs, CUDA-event timed (GPU-only). Establishes the parity target our CKIR FFT must match on raw 1D FFT (the
// crush itself is the fused 2D convolution, Phase 3). Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by
// scripts/run_bench_gpu_cuda.sh.
//
// FLOP model: a radix-2 complex FFT is the standard 5*N*log2(N) flops per transform (Cooley-Tukey), * batch.
// We report cuFFT's BEST (min) time = its strongest number = the most conservative parity bar for us.

#include "cuda_bench.hpp"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cmath>
#include <cstdio>

int main()
{
    const long long total = 1LL << 24; // ~16.7M complex elements total per size (saturates the GPU)
    const int       sizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 65536, 262144};

    int            dev = 0;
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDevice(&dev));
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, dev));
    std::printf("# cuFFT C2C forward (f32) — %s, CUDA runtime\n", prop.name);
    std::printf("# total complex elements per row ~ %lld\n", total);
    std::printf("# %-8s %-10s %-10s %-10s\n", "N", "batch", "min_ms", "GFLOP/s");

    for (const int n : sizes)
    {
        const int     batch = static_cast<int>(total / n);
        const size_t  bytes = sizeof(cufftComplex) * static_cast<size_t>(n) * static_cast<size_t>(batch);
        cufftComplex* d     = nullptr;
        CRD_CUDA_CHECK(cudaMalloc(&d, bytes));
        CRD_CUDA_CHECK(cudaMemset(d, 0, bytes));

        cufftHandle plan;
        CRD_CUDA_CHECK(cufftPlan1d(&plan, n, CUFFT_C2C, batch));

        for (int i = 0; i < 5; ++i) // warmup
        {
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_FORWARD));
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());

        const float best = crd_bench::best_event_ms(30, [&]
        {
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_FORWARD));
        });
        const double points = static_cast<double>(n);
        const double flops  = 5.0 * points * std::log2(points) * static_cast<double>(batch);
        const double gflops = flops / (static_cast<double>(best) * 1e-3) / 1e9;
        std::printf("%-8d %-10d %-10.4f %-10.1f\n", n, batch, static_cast<double>(best), gflops);

        CRD_CUDA_CHECK(cufftDestroy(plan));
        CRD_CUDA_CHECK(cudaFree(d));
    }
    return 0;
}
