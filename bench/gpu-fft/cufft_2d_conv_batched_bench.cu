// cufft_2d_conv_batched_bench.cu — GPU FFT crush campaign: the VENDOR BATCHED 2-D CONVOLUTION baseline. B contiguous images
// share ONE PSF spectrum; the vendor path is cufftExecC2C (batched 2-D fwd) -> elementwise-multiply (broadcast one filter over
// B images) -> cufftExecC2C (batched 2-D inv). This is the DRAM-BOUND regime: once B·image > L2 (Ada ~32-48 MB, one 1024²
// complex image = 8 MB, so B>=8 spills L2), the vendor pays its multi-pass 2-D FFT entirely out of DRAM. Our FUSED pipeline is
// THREE global passes (row FFT / fused column conv+multiply+inv / inv row FFT); the vendor's 2-D fwd+mul+inv is ~5. Fewer
// DRAM round-trips ⇒ our fusion crushes cuFFT exactly here (the 1-D story: 1.99× when DRAM-bound). This measures the vendor
// end-to-end time for the WHOLE batch; compare to the fused Vulkan number ([.fft2dconv-batched]).
// Build: nvcc -O3 -allow-unsupported-compiler cufft_2d_conv_batched_bench.cu -o cufft_2d_conv_batched_bench.exe -lcufft

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

// a[b*rc + i] *= f[i]  (one filter spectrum broadcast over all B images)
__global__ void cmul_bcast(cufftComplex* a, const cufftComplex* f, long long rc, long long total)
{
    long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < total)
    {
        cufftComplex x = a[i];
        cufftComplex y = f[i % rc];
        a[i].x         = x.x * y.x - x.y * y.y;
        a[i].y         = x.x * y.y + x.y * y.x;
    }
}

int main()
{
    const int      n        = 1024;               // n×n images (power of 4 crush point)
    const int      batches[] = {1, 4, 8, 16, 32}; // B: L2 spills at B>=8 (8 MB/image, Ada L2 ~32-48 MB)
    const long long rc       = (long long)n * n;  // one image
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# cuFFT BATCHED 2-D CONVOLUTION, %d×%d, whole-batch time (batched fwd + broadcast multiply + batched inv) — %s\n", n, n, prop.name);
    std::printf("# %-6s %-14s %-16s\n", "B", "batch_ms", "per_image_ms");

    for (int bi = 0; bi < 5; ++bi)
    {
        const int       B   = batches[bi];
        const long long tot = rc * B;
        cufftComplex*   d   = nullptr; // B images (transformed in place)
        cufftComplex*   f   = nullptr; // ONE PSF spectrum
        cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMalloc(&f, sizeof(cufftComplex) * static_cast<size_t>(rc));
        cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMemset(f, 0, sizeof(cufftComplex) * static_cast<size_t>(rc));

        // batched 2-D plan: B transforms of an n×n image, contiguous (idist = odist = rc).
        cufftHandle plan;
        int         dims[2] = {n, n};
        if (cufftPlanMany(&plan, 2, dims, nullptr, 1, static_cast<int>(rc), nullptr, 1, static_cast<int>(rc), CUFFT_C2C, B) != CUFFT_SUCCESS)
        {
            std::printf("# plan fail B=%d\n", B);
            cudaFree(d);
            cudaFree(f);
            continue;
        }

        const int block = 256;
        const int grid  = static_cast<int>((tot + block - 1) / block);
        for (int i = 0; i < 5; ++i) // warmup
        {
            cufftExecC2C(plan, d, d, CUFFT_FORWARD);
            cmul_bcast<<<grid, block>>>(d, f, rc, tot);
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
            cufftExecC2C(plan, d, d, CUFFT_FORWARD);       // batched 2-D forward
            cmul_bcast<<<grid, block>>>(d, f, rc, tot);    // ×PSF (broadcast)
            cufftExecC2C(plan, d, d, CUFFT_INVERSE);        // batched 2-D inverse
            cudaEventRecord(e1);
            cudaEventSynchronize(e1);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, e0, e1);
            if (ms < best) { best = ms; }
        }
        std::printf("%-6d %-14.4f %-16.5f\n", B, best, best / B);
        cufftDestroy(plan);
        cudaFree(d);
        cudaFree(f);
        cudaEventDestroy(e0);
        cudaEventDestroy(e1);
    }
    return 0;
}
