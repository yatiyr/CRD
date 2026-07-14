// cufft_conv_bench.cu — GPU FFT crush campaign: the VENDOR CONVOLUTION baseline (the thing our FUSED FFT-convolution crushes).
// A cuFFT-based circular convolution is THREE global passes: cufftExecC2C(fwd) → elementwise-multiply kernel → cufftExecC2C
// (inv). Each pass round-trips N*batch complex through global memory. Our fused kernel does the whole thing in ONE on-chip
// dispatch (FFT→×spectrum→iFFT in shared). This measures the vendor end-to-end time; compare to the fused Vulkan number.
// Build: nvcc -O3 -allow-unsupported-compiler cufft_conv_bench.cu -o cufft_conv_bench.exe -lcufft

#include <cuda_runtime.h>
#include <cufft.h>

#include <cmath>
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
    const long long total_elems = 1LL << 24;
    const int       sizes[]     = {256, 512, 1024, 2048, 4096};
    cudaDeviceProp  prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# cuFFT CONVOLUTION (fwd + multiply + inv = 3 global passes) — %s\n", prop.name);
    std::printf("# %-8s %-10s %-12s\n", "N", "batch", "conv_min_ms");

    for (int si = 0; si < 5; ++si)
    {
        const int       n     = sizes[si];
        const int       batch = static_cast<int>(total_elems / n);
        const long long tot   = static_cast<long long>(n) * batch;
        cufftComplex*   d     = nullptr; // signal (transformed in place)
        cufftComplex*   f     = nullptr; // filter spectrum
        cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMalloc(&f, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMemset(f, 0, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cufftHandle plan;
        if (cufftPlan1d(&plan, n, CUFFT_C2C, batch) != CUFFT_SUCCESS) { std::printf("# plan fail N=%d\n", n); continue; }

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
            cufftExecC2C(plan, d, d, CUFFT_FORWARD); // pass 1
            cmul<<<grid, block>>>(d, f, tot);        // pass 2 (×spectrum)
            cufftExecC2C(plan, d, d, CUFFT_INVERSE);  // pass 3
            cudaEventRecord(e1);
            cudaEventSynchronize(e1);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, e0, e1);
            if (ms < best) { best = ms; }
        }
        std::printf("%-8d %-10d %-12.4f\n", n, batch, best);
        cufftDestroy(plan);
        cudaFree(d);
        cudaFree(f);
        cudaEventDestroy(e0);
        cudaEventDestroy(e1);
    }
    return 0;
}
