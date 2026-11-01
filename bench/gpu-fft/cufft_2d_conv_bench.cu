// cufft_2d_conv_bench.cu — GPU FFT crush campaign: the VENDOR 2-D CONVOLUTION baseline (the thing our FUSED 2-D FFT-
// convolution crushes). A cuFFT-based 2-D circular convolution is THREE global passes over the whole image: cufftExecC2C
// (2-D fwd) -> elementwise-multiply kernel -> cufftExecC2C (2-D inv). Each 2-D FFT internally round-trips the image through
// global memory MULTIPLE times (row pass + transpose + column pass); the vendor pays the column dimension three separate
// times (fwd, multiply, inv). Our fused pipeline keeps the column FFT + multiply + inverse column FFT in ONE on-chip
// dispatch. This measures the vendor end-to-end per-image time; compare to the fused Vulkan number ([.fft2dconv-bench]).
// This is the bloom workload: ONE image per frame (batch = 1), the realistic per-frame convolution.
// Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh.

#include "cuda_bench.hpp"
#include "spectrum_multiply.cuh"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

int main()
{
    const int      sizes[] = {256, 512, 1024, 2048}; // n×n images (256, 1024 are our power-of-4 crush points)
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# cuFFT 2-D CONVOLUTION per image (2-D fwd + multiply + 2-D inv = 3 global passes) — %s\n", prop.name);
    std::printf("# %-8s %-14s\n", "N(=NxN)", "conv_min_ms");

    for (const int n : sizes)
    {
        const long long tot   = static_cast<long long>(n) * n; // one n×n image
        const size_t    bytes = sizeof(cufftComplex) * static_cast<size_t>(tot);
        cufftComplex*   d     = nullptr; // image (transformed in place)
        cufftComplex*   f     = nullptr; // PSF spectrum
        CRD_CUDA_CHECK(cudaMalloc(&d, bytes));
        CRD_CUDA_CHECK(cudaMalloc(&f, bytes));
        CRD_CUDA_CHECK(cudaMemset(d, 0, bytes));
        CRD_CUDA_CHECK(cudaMemset(f, 0, bytes));
        cufftHandle plan;
        CRD_CUDA_CHECK(cufftPlan2d(&plan, n, n, CUFFT_C2C));

        const auto convolve = [&]
        {
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_FORWARD)); // pass 1: full 2-D forward FFT
            crd_bench::multiply_spectrum(d, f, tot);                 // pass 2: ×PSF spectrum (whole image)
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_INVERSE)); // pass 3: full 2-D inverse FFT
        };
        for (int i = 0; i < 5; ++i) // warmup
        {
            convolve();
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const float best = crd_bench::best_event_ms(30, convolve);
        std::printf("%-8d %-14.4f\n", n, static_cast<double>(best));
        CRD_CUDA_CHECK(cufftDestroy(plan));
        CRD_CUDA_CHECK(cudaFree(d));
        CRD_CUDA_CHECK(cudaFree(f));
    }
    return 0;
}
