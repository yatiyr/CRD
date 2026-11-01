// cufft_2d_conv_r2c_batched_bench.cu — GPU FFT crush campaign: the VENDOR BATCHED **REAL** 2-D CONVOLUTION baseline. A real
// image + real PSF has a Hermitian spectrum, so the vendor's real path is cufftExecR2C (real → n×(n/2+1) complex half) →
// broadcast-multiply one half PSF over B images → cufftExecC2R (half → real). ~Half the work + traffic of the C2C path. This
// is the honest peer for our R2C fused conv ([.fft2dconv-r2c]); once B·image spills L2 (B>=8) both go DRAM-bound and our fewer
// round-trips (3 passes vs the vendor's ~5) win. Whole-batch time, min-of-30.
// Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh.

#include "cuda_bench.hpp"
#include "spectrum_multiply.cuh"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

int main()
{
    const int       n         = 1024; // n×n real images
    const int       batches[] = {1, 4, 8, 16, 32};
    const long long rr        = static_cast<long long>(n) * n;           // real elements per image
    const long long hc        = static_cast<long long>(n) * (n / 2 + 1); // complex half-spectrum elements per image
    cudaDeviceProp  prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# cuFFT BATCHED REAL 2-D CONVOLUTION, %d×%d (R2C fwd + broadcast multiply + C2R inv) — %s\n", n, n, prop.name);
    std::printf("# %-6s %-14s %-16s\n", "B", "batch_ms", "per_image_ms");

    for (const int b : batches)
    {
        const long long rtot   = rr * b;
        const long long ctot   = hc * b;
        float*          d_real = nullptr; // B real images (in) / real outputs
        cufftComplex*   d_cplx = nullptr; // B half spectra
        cufftComplex*   f      = nullptr; // ONE half PSF spectrum
        CRD_CUDA_CHECK(cudaMalloc(&d_real, sizeof(float) * static_cast<size_t>(rtot)));
        CRD_CUDA_CHECK(cudaMalloc(&d_cplx, sizeof(cufftComplex) * static_cast<size_t>(ctot)));
        CRD_CUDA_CHECK(cudaMalloc(&f, sizeof(cufftComplex) * static_cast<size_t>(hc)));
        CRD_CUDA_CHECK(cudaMemset(d_real, 0, sizeof(float) * static_cast<size_t>(rtot)));
        CRD_CUDA_CHECK(cudaMemset(d_cplx, 0, sizeof(cufftComplex) * static_cast<size_t>(ctot)));
        CRD_CUDA_CHECK(cudaMemset(f, 0, sizeof(cufftComplex) * static_cast<size_t>(hc)));

        int         dims[2] = {n, n};
        cufftHandle plan_r2c;
        cufftHandle plan_c2r;
        // R2C: real idist = n*n → complex odist = n*(n/2+1). C2R: the inverse.
        CRD_CUDA_CHECK(cufftPlanMany(&plan_r2c, 2, dims, nullptr, 1, static_cast<int>(rr), nullptr, 1,
                                     static_cast<int>(hc), CUFFT_R2C, b));
        CRD_CUDA_CHECK(cufftPlanMany(&plan_c2r, 2, dims, nullptr, 1, static_cast<int>(hc), nullptr, 1,
                                     static_cast<int>(rr), CUFFT_C2R, b));

        const auto convolve = [&]
        {
            CRD_CUDA_CHECK(cufftExecR2C(plan_r2c, d_real, d_cplx));    // real → half spectrum
            crd_bench::multiply_spectrum_broadcast(d_cplx, f, hc, ctot); // ×half PSF (broadcast)
            CRD_CUDA_CHECK(cufftExecC2R(plan_c2r, d_cplx, d_real));    // half spectrum → real
        };
        for (int i = 0; i < 5; ++i) // warmup
        {
            convolve();
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const float best = crd_bench::best_event_ms(30, convolve);
        std::printf("%-6d %-14.4f %-16.5f\n", b, static_cast<double>(best), static_cast<double>(best) / b);
        CRD_CUDA_CHECK(cufftDestroy(plan_r2c));
        CRD_CUDA_CHECK(cufftDestroy(plan_c2r));
        CRD_CUDA_CHECK(cudaFree(d_real));
        CRD_CUDA_CHECK(cudaFree(d_cplx));
        CRD_CUDA_CHECK(cudaFree(f));
    }
    return 0;
}
