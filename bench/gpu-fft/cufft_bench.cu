// cufft_bench.cu — GPU FFT crush campaign (D-007 B-cmp): the cuFFT GOLD-STANDARD peer harness. Batched C2C forward FFT,
// min-of-N runs, CUDA-event timed (GPU-only). Establishes the parity target our CKIR FFT must match on raw 1D FFT (the
// crush itself is the fused 2D convolution, Phase 3). Re-runnable: nvcc -O3 cufft_bench.cu -o cufft_bench.exe -lcufft.
//
// FLOP model: a radix-2 complex FFT is the standard 5*N*log2(N) flops per transform (Cooley-Tukey), * batch.
// We report cuFFT's BEST (min) time = its strongest number = the most conservative parity bar for us.

#include <cuda_runtime.h>
#include <cufft.h>

#include <cmath>
#include <cstdio>

int main()
{
    const long long total = 1LL << 24; // ~16.7M complex elements total per size (saturates the GPU)
    const int       sizes[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 65536, 262144};
    const int       nsizes  = static_cast<int>(sizeof(sizes) / sizeof(sizes[0]));

    int dev = 0;
    cudaDeviceProp prop{};
    cudaGetDevice(&dev);
    cudaGetDeviceProperties(&prop, dev);
    std::printf("# cuFFT C2C forward (f32) — %s, CUDA runtime\n", prop.name);
    std::printf("# total complex elements per row ~ %lld\n", total);
    std::printf("# %-8s %-10s %-10s %-10s\n", "N", "batch", "min_ms", "GFLOP/s");

    for (int si = 0; si < nsizes; ++si)
    {
        const int    n     = sizes[si];
        const int    batch = static_cast<int>(total / n);
        cufftComplex* d     = nullptr;
        if (cudaMalloc(&d, sizeof(cufftComplex) * static_cast<size_t>(n) * static_cast<size_t>(batch)) != cudaSuccess)
        {
            std::printf("# alloc fail N=%d\n", n);
            continue;
        }
        cudaMemset(d, 0, sizeof(cufftComplex) * static_cast<size_t>(n) * static_cast<size_t>(batch));

        cufftHandle plan;
        if (cufftPlan1d(&plan, n, CUFFT_C2C, batch) != CUFFT_SUCCESS)
        {
            std::printf("# plan fail N=%d\n", n);
            cudaFree(d);
            continue;
        }

        for (int i = 0; i < 5; ++i) { cufftExecC2C(plan, d, d, CUFFT_FORWARD); } // warmup
        cudaDeviceSynchronize();

        cudaEvent_t ev0;
        cudaEvent_t ev1;
        cudaEventCreate(&ev0);
        cudaEventCreate(&ev1);
        float best = 1e30f;
        for (int r = 0; r < 30; ++r) // min-of-30
        {
            cudaEventRecord(ev0);
            cufftExecC2C(plan, d, d, CUFFT_FORWARD);
            cudaEventRecord(ev1);
            cudaEventSynchronize(ev1);
            float ms = 0.0f;
            cudaEventElapsedTime(&ms, ev0, ev1);
            if (ms < best) { best = ms; }
        }
        const double flops  = 5.0 * static_cast<double>(n) * (std::log(static_cast<double>(n)) / std::log(2.0)) * static_cast<double>(batch);
        const double gflops = flops / (static_cast<double>(best) * 1e-3) / 1e9;
        std::printf("%-8d %-10d %-10.4f %-10.1f\n", n, batch, best, gflops);

        cufftDestroy(plan);
        cudaFree(d);
        cudaEventDestroy(ev0);
        cudaEventDestroy(ev1);
    }
    return 0;
}
