// cufft_2d_conv_r2c_batched_bench.cu — GPU FFT crush campaign: the VENDOR BATCHED **REAL** 2-D CONVOLUTION baseline. A real
// image + real PSF has a Hermitian spectrum, so the vendor's real path is cufftExecR2C (real → n×(n/2+1) complex half) →
// broadcast-multiply one half PSF over B images → cufftExecC2R (half → real). ~Half the work + traffic of the C2C path. This
// is the honest peer for our R2C fused conv ([.fft2dconv-r2c]); once B·image spills L2 (B>=8) both go DRAM-bound and our fewer
// round-trips (3 passes vs the vendor's ~5) win. Whole-batch time, min-of-30.
// Build: nvcc -O3 -allow-unsupported-compiler cufft_2d_conv_r2c_batched_bench.cu -o cufft_2d_conv_r2c_batched_bench.exe -lcufft

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

// a[b*hc + i] *= f[i]  (one half PSF spectrum broadcast over all B images; hc = n*(n/2+1) complex per image)
__global__ void cmul_bcast(cufftComplex* a, const cufftComplex* f, long long hc, long long total)
{
    long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < total)
    {
        cufftComplex x = a[i];
        cufftComplex y = f[i % hc];
        a[i].x         = x.x * y.x - x.y * y.y;
        a[i].y         = x.x * y.y + x.y * y.x;
    }
}

int main()
{
    const int       n         = 1024;               // n×n real images
    const int       batches[] = {1, 4, 8, 16, 32};
    const long long rr        = (long long)n * n;        // real elements per image
    const long long hc        = (long long)n * (n / 2 + 1); // complex half-spectrum elements per image
    cudaDeviceProp  prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# cuFFT BATCHED REAL 2-D CONVOLUTION, %d×%d (R2C fwd + broadcast multiply + C2R inv) — %s\n", n, n, prop.name);
    std::printf("# %-6s %-14s %-16s\n", "B", "batch_ms", "per_image_ms");

    for (int bi = 0; bi < 5; ++bi)
    {
        const int       B     = batches[bi];
        const long long rtot  = rr * B;
        const long long ctot  = hc * B;
        float*          d_real = nullptr;      // B real images (in) / real outputs
        cufftComplex*   d_cplx = nullptr;      // B half spectra
        cufftComplex*   f      = nullptr;      // ONE half PSF spectrum
        cudaMalloc(&d_real, sizeof(float) * static_cast<size_t>(rtot));
        cudaMalloc(&d_cplx, sizeof(cufftComplex) * static_cast<size_t>(ctot));
        cudaMalloc(&f, sizeof(cufftComplex) * static_cast<size_t>(hc));
        cudaMemset(d_real, 0, sizeof(float) * static_cast<size_t>(rtot));
        cudaMemset(d_cplx, 0, sizeof(cufftComplex) * static_cast<size_t>(ctot));
        cudaMemset(f, 0, sizeof(cufftComplex) * static_cast<size_t>(hc));

        int dims[2] = {n, n};
        cufftHandle planR2C;
        cufftHandle planC2R;
        // R2C: real idist = n*n → complex odist = n*(n/2+1). C2R: the inverse.
        if (cufftPlanMany(&planR2C, 2, dims, nullptr, 1, static_cast<int>(rr), nullptr, 1, static_cast<int>(hc), CUFFT_R2C, B) != CUFFT_SUCCESS ||
            cufftPlanMany(&planC2R, 2, dims, nullptr, 1, static_cast<int>(hc), nullptr, 1, static_cast<int>(rr), CUFFT_C2R, B) != CUFFT_SUCCESS)
        {
            std::printf("# plan fail B=%d\n", B);
            cudaFree(d_real); cudaFree(d_cplx); cudaFree(f);
            continue;
        }

        const int block = 256;
        const int grid  = static_cast<int>((ctot + block - 1) / block);
        for (int i = 0; i < 5; ++i) // warmup
        {
            cufftExecR2C(planR2C, d_real, d_cplx);
            cmul_bcast<<<grid, block>>>(d_cplx, f, hc, ctot);
            cufftExecC2R(planC2R, d_cplx, d_real);
        }
        cudaDeviceSynchronize();

        cudaEvent_t e0;
        cudaEvent_t e1;
        cudaEventCreate(&e0);
        cudaEventCreate(&e1);
        float best = 1e30f;
        for (int r = 0; r < 30; ++r)
        {
            cudaEventRecord(e0);
            cufftExecR2C(planR2C, d_real, d_cplx);        // real → half spectrum
            cmul_bcast<<<grid, block>>>(d_cplx, f, hc, ctot); // ×half PSF (broadcast)
            cufftExecC2R(planC2R, d_cplx, d_real);         // half spectrum → real
            cudaEventRecord(e1);
            cudaEventSynchronize(e1);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, e0, e1);
            if (ms < best) { best = ms; }
        }
        std::printf("%-6d %-14.4f %-16.5f\n", B, best, best / B);
        cufftDestroy(planR2C);
        cufftDestroy(planC2R);
        cudaFree(d_real); cudaFree(d_cplx); cudaFree(f);
        cudaEventDestroy(e0);
        cudaEventDestroy(e1);
    }
    return 0;
}
