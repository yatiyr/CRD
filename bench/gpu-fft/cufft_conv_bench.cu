// cufft_conv_bench.cu — GPU FFT crush campaign: the VENDOR CONVOLUTION baseline (the thing our FUSED FFT-convolution crushes).
// A cuFFT-based circular convolution is THREE global passes: cufftExecC2C(fwd) → elementwise-multiply kernel → cufftExecC2C
// (inv). Each pass round-trips N*batch complex through global memory. Our fused kernel does the whole thing in ONE on-chip
// dispatch (FFT→×spectrum→iFFT in shared). This measures the vendor end-to-end time; compare to the fused Vulkan number.
// Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh.

#include "cuda_bench.hpp"
#include "spectrum_multiply.cuh"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

int main()
{
    const long long total_elems = 1LL << 24;
    const int       sizes[]     = {256, 512, 1024, 2048, 4096};
    cudaDeviceProp  prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# cuFFT CONVOLUTION (fwd + multiply + inv = 3 global passes) — %s\n", prop.name);
    std::printf("# %-8s %-10s %-12s\n", "N", "batch", "conv_min_ms");

    for (const int n : sizes)
    {
        const int       batch = static_cast<int>(total_elems / n);
        const long long tot   = static_cast<long long>(n) * batch;
        const size_t    bytes = sizeof(cufftComplex) * static_cast<size_t>(tot);
        cufftComplex*   d     = nullptr; // signal (transformed in place)
        cufftComplex*   f     = nullptr; // filter spectrum
        CRD_CUDA_CHECK(cudaMalloc(&d, bytes));
        CRD_CUDA_CHECK(cudaMalloc(&f, bytes));
        CRD_CUDA_CHECK(cudaMemset(d, 0, bytes));
        CRD_CUDA_CHECK(cudaMemset(f, 0, bytes));
        cufftHandle plan;
        CRD_CUDA_CHECK(cufftPlan1d(&plan, n, CUFFT_C2C, batch));

        const auto convolve = [&]
        {
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_FORWARD)); // pass 1
            crd_bench::multiply_spectrum(d, f, tot);                 // pass 2 (×spectrum)
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_INVERSE)); // pass 3
        };
        for (int i = 0; i < 5; ++i) // warmup
        {
            convolve();
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const float best = crd_bench::best_event_ms(30, convolve);
        std::printf("%-8d %-10d %-12.4f\n", n, batch, static_cast<double>(best));
        CRD_CUDA_CHECK(cufftDestroy(plan));
        CRD_CUDA_CHECK(cudaFree(d));
        CRD_CUDA_CHECK(cudaFree(f));
    }
    return 0;
}
