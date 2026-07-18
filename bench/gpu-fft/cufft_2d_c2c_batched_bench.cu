// cufft_2d_c2c_batched_bench.cu — B16-a-2 FFT-ocean crush baseline: the VENDOR BATCHED 2-D complex-to-complex INVERSE FFT.
// This is the exact peer for our build_fft2d_c2c_batched (the ocean transforms its 4 packed complex fields — height/
// displacement/slope/Jacobian — in ONE batched inverse 2-D FFT; with C cascades the batch is 4·C). cuFFT does a batched 2-D
// C2C inverse via cufftPlanMany + cufftExecC2C(CUFFT_INVERSE). Both sides are UNNORMALISED (the ocean folds no 1/N²). The
// crush lever is the same as the whole campaign: once the batch·image working set spills L2 (Ada ~32-48 MB), the transform is
// DRAM-BOUND and our 2-pass transpose-on-write strided IFFT (row IFFT + strided column IFFT = fewer global round-trips) beats
// cuFFT's transpose-based multipass. Below the L2 spill it is compute/L2-bound and bit-exactness (no FMA) costs us — parity.
// Compare per-batch ms to the Vulkan [.ocean-ifft-bench] number (last_gpu_ms, same n/batch).
// Build: nvcc -O3 -allow-unsupported-compiler cufft_2d_c2c_batched_bench.cu -o cufft_2d_c2c_batched_bench.exe -lcufft

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

static void sweep(int n)
{
    const int       batches[] = {4, 8, 16, 32, 64}; // 4 packed ocean fields × {1,2,4,8,16} cascades
    const long long rc        = (long long)n * n;   // one image (complex elements)
    std::printf("# cuFFT BATCHED 2-D C2C INVERSE, %dx%d, whole-batch time (batched inverse 2-D FFT), image = %.2f MB\n",
                n, n, (double)(rc * (long long)sizeof(cufftComplex)) / (1024.0 * 1024.0));
    std::printf("# %-6s %-14s %-16s %-12s\n", "B", "batch_ms", "per_image_ms", "workset_MB");
    for (int bi = 0; bi < 5; ++bi)
    {
        const int       B   = batches[bi];
        const long long tot = rc * B;
        cufftComplex*   d   = nullptr;
        cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(tot));
        cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(tot));

        cufftHandle plan;
        int         dims[2] = {n, n};
        if (cufftPlanMany(&plan, 2, dims, nullptr, 1, static_cast<int>(rc), nullptr, 1, static_cast<int>(rc), CUFFT_C2C, B) != CUFFT_SUCCESS)
        {
            std::printf("# plan fail B=%d\n", B);
            cudaFree(d);
            continue;
        }
        for (int i = 0; i < 5; ++i) { cufftExecC2C(plan, d, d, CUFFT_INVERSE); } // warmup
        cudaDeviceSynchronize();

        cudaEvent_t e0;
        cudaEvent_t e1;
        cudaEventCreate(&e0);
        cudaEventCreate(&e1);
        float best = 1e30f;
        for (int r = 0; r < 30; ++r) // min-of-30, kernel-only (cudaEvent brackets just the transform)
        {
            cudaEventRecord(e0);
            cufftExecC2C(plan, d, d, CUFFT_INVERSE);
            cudaEventRecord(e1);
            cudaEventSynchronize(e1);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, e0, e1);
            if (ms < best) { best = ms; }
        }
        const double workset = (double)(tot * (long long)sizeof(cufftComplex)) / (1024.0 * 1024.0);
        std::printf("%-6d %-14.4f %-16.5f %-12.2f\n", B, best, best / B, workset);
        cufftDestroy(plan);
        cudaFree(d);
        cudaEventDestroy(e0);
        cudaEventDestroy(e1);
    }
    std::printf("\n");
}

int main()
{
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# device: %s, L2 %.1f MB, bus %d-bit\n", prop.name, (double)prop.l2CacheSize / (1024.0 * 1024.0),
                prop.memoryBusWidth);
    sweep(256);
    sweep(512);
    return 0;
}
