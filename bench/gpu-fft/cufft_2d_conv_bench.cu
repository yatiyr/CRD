// cufft_2d_conv_bench.cu — GPU FFT crush campaign: the VENDOR 2-D CONVOLUTION baseline (the thing our FUSED 2-D FFT-
// convolution crushes). A cuFFT-based 2-D circular convolution is THREE global passes over the whole image: cufftExecC2C
// (2-D fwd) -> elementwise-multiply kernel -> cufftExecC2C (2-D inv). Each 2-D FFT internally round-trips the image through
// global memory MULTIPLE times (row pass + transpose + column pass); the vendor pays the column dimension three separate
// times (fwd, multiply, inv). Our fused pipeline keeps the column FFT + multiply + inverse column FFT in ONE on-chip
// dispatch. This measures the vendor end-to-end per-image time; compare to the fused Vulkan number ([.fft2dconv-bench]).
// This is the bloom workload: ONE image per frame (batch = 1), the realistic per-frame convolution.
// Build: nvcc -O3 -allow-unsupported-compiler cufft_2d_conv_bench.cu -o cufft_2d_conv_bench.exe -lcufft

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

__global__ void cmul(cufftComplex* a, const cufftComplex* b, long long total)
{
    long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < total)
    {
        cufftComplex x = a[i];
        cufftComplex y = b[i];
        a[i].x         = x.x * y.x - x.y * y.y;
        a[i].y         = x.x * y.y + x.y * y.x;
    }
}

int main()
{
    const int      sizes[] = {256, 512, 1024, 2048}; // n×n images (256, 1024 are our power-of-4 crush points)
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# cuFFT 2-D CONVOLUTION per image (2-D fwd + multiply + 2-D inv = 3 global passes) — %s\n", prop.name);
    std::printf("# %-8s %-14s\n", "N(=NxN)", "conv_min_ms");

    for (int si = 0; si < 4; ++si)
    {
        const int       n   = sizes[si];
        const long long tot = static_cast<long long>(n) * n; // one n×n image
        cufftComplex*   d   = nullptr;                        // image (transformed in place)
        cufftComplex*   f   = nullptr;                        // PSF spectrum
        cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMalloc(&f, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMemset(f, 0, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cufftHandle plan;
        if (cufftPlan2d(&plan, n, n, CUFFT_C2C) != CUFFT_SUCCESS) { std::printf("# plan fail N=%d\n", n); continue; }

        const int block = 256;
        const int grid  = static_cast<int>((tot + block - 1) / block);
        for (int i = 0; i < 5; ++i) // warmup
        {
            cufftExecC2C(plan, d, d, CUFFT_FORWARD);
            cmul<<<grid, block>>>(d, f, tot);
            cufftExecC2C(plan, d, d, CUFFT_INVERSE);
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
            cufftExecC2C(plan, d, d, CUFFT_FORWARD); // pass 1: full 2-D forward FFT
            cmul<<<grid, block>>>(d, f, tot);        // pass 2: ×PSF spectrum (whole image)
            cufftExecC2C(plan, d, d, CUFFT_INVERSE);  // pass 3: full 2-D inverse FFT
            cudaEventRecord(e1);
            cudaEventSynchronize(e1);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, e0, e1);
            if (ms < best) { best = ms; }
        }
        std::printf("%-8d %-14.4f\n", n, best);
        cufftDestroy(plan);
        cudaFree(d);
        cudaFree(f);
        cudaEventDestroy(e0);
        cudaEventDestroy(e1);
    }
    return 0;
}
