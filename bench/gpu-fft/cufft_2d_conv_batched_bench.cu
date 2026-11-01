// cufft_2d_conv_batched_bench.cu — GPU FFT crush campaign: the VENDOR BATCHED 2-D CONVOLUTION baseline. B contiguous images
// share ONE PSF spectrum; the vendor path is cufftExecC2C (batched 2-D fwd) -> elementwise-multiply (broadcast one filter over
// B images) -> cufftExecC2C (batched 2-D inv). This is the DRAM-BOUND regime: once B·image > L2 (Ada ~32-48 MB, one 1024²
// complex image = 8 MB, so B>=8 spills L2), the vendor pays its multi-pass 2-D FFT entirely out of DRAM. Our FUSED pipeline is
// THREE global passes (row FFT / fused column conv+multiply+inv / inv row FFT); the vendor's 2-D fwd+mul+inv is ~5. Fewer
// DRAM round-trips ⇒ our fusion crushes cuFFT exactly here (the 1-D story: 1.99× when DRAM-bound). This measures the vendor
// end-to-end time for the WHOLE batch; compare to the fused Vulkan number ([.fft2dconv-batched]).
// Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh.

#include "cuda_bench.hpp"
#include "spectrum_multiply.cuh"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

int main()
{
    const int       n         = 1024;                         // n×n images (power of 4 crush point)
    const int       batches[] = {1, 4, 8, 16, 32};             // B: L2 spills at B>=8 (8 MB/image, Ada L2 ~32-48 MB)
    const long long rc        = static_cast<long long>(n) * n; // one image
    cudaDeviceProp  prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# cuFFT BATCHED 2-D CONVOLUTION, %d×%d, whole-batch time (batched fwd + broadcast multiply + batched inv) — %s\n", n, n, prop.name);
    std::printf("# %-6s %-14s %-16s\n", "B", "batch_ms", "per_image_ms");

    for (const int b : batches)
    {
        const long long tot = rc * b;
        cufftComplex*   d   = nullptr; // B images (transformed in place)
        cufftComplex*   f   = nullptr; // ONE PSF spectrum
        CRD_CUDA_CHECK(cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(tot)));
        CRD_CUDA_CHECK(cudaMalloc(&f, sizeof(cufftComplex) * static_cast<size_t>(rc)));
        CRD_CUDA_CHECK(cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(tot)));
        CRD_CUDA_CHECK(cudaMemset(f, 0, sizeof(cufftComplex) * static_cast<size_t>(rc)));

        // batched 2-D plan: B transforms of an n×n image, contiguous (idist = odist = rc).
        cufftHandle plan;
        int         dims[2] = {n, n};
        CRD_CUDA_CHECK(cufftPlanMany(&plan, 2, dims, nullptr, 1, static_cast<int>(rc), nullptr, 1, static_cast<int>(rc),
                                     CUFFT_C2C, b));

        const auto convolve = [&]
        {
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_FORWARD));  // batched 2-D forward
            crd_bench::multiply_spectrum_broadcast(d, f, rc, tot);    // ×PSF (broadcast)
            CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_INVERSE));  // batched 2-D inverse
        };
        for (int i = 0; i < 5; ++i) // warmup
        {
            convolve();
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const float best = crd_bench::best_event_ms(30, convolve);
        std::printf("%-6d %-14.4f %-16.5f\n", b, static_cast<double>(best), static_cast<double>(best) / b);
        CRD_CUDA_CHECK(cufftDestroy(plan));
        CRD_CUDA_CHECK(cudaFree(d));
        CRD_CUDA_CHECK(cudaFree(f));
    }
    return 0;
}
