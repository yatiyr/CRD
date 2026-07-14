// cub_scan_bench.cu — B-cmp: the VENDOR prefix-sum baseline. CUB `DeviceScan::InclusiveSum` is NVIDIA's production scan — a
// SINGLE-PASS decoupled-look-back kernel (reads N once, writes N once ≈ 2N traffic, using device-scope atomics + spin for the
// inter-block prefix). Our PORTABLE 3-pass scan (no atomics ⇒ deterministic + bit-exact on Vulkan/DX12) pays ~4N traffic, so
// this is the honest bar for a portable scan. Memory-bound ⇒ the metric is bandwidth. Build with -arch=sm_89 (driver rejects
// 13.3 PTX JIT) + -std=c++17 -Xcompiler /Zc:preprocessor -I ...\include\cccl.

#include <cuda_runtime.h>
#include <cub/device/device_scan.cuh>

#include <chrono>
#include <cstdio>

int main()
{
    const int      sizes[] = {1 << 22, 1 << 24}; // 16 MB (L2) · 64 MB (DRAM-bound)
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# CUB DeviceScan::InclusiveSum (f32) — %s\n", prop.name);
    std::printf("# %-10s %-12s %-12s\n", "N", "min_ms", "GB/s(2N)");

    for (int si = 0; si < 2; ++si)
    {
        const int n = sizes[si];
        float*    d_in  = nullptr;
        float*    d_out = nullptr;
        cudaMalloc(&d_in, sizeof(float) * static_cast<size_t>(n));
        cudaMalloc(&d_out, sizeof(float) * static_cast<size_t>(n));
        {
            float* h = static_cast<float*>(malloc(sizeof(float) * static_cast<size_t>(n)));
            for (int i = 0; i < n; ++i) { h[i] = 1.0f; }
            cudaMemcpy(d_in, h, sizeof(float) * static_cast<size_t>(n), cudaMemcpyHostToDevice);
            free(h);
        }

        void*  d_temp     = nullptr;
        size_t temp_bytes = 0;
        cub::DeviceScan::InclusiveSum(d_temp, temp_bytes, d_in, d_out, n);
        cudaMalloc(&d_temp, temp_bytes);
        cub::DeviceScan::InclusiveSum(d_temp, temp_bytes, d_in, d_out, n);
        cudaDeviceSynchronize();
        float last = 0.0f;
        cudaMemcpy(&last, d_out + (n - 1), sizeof(float), cudaMemcpyDeviceToHost);
        std::printf("# N=%d temp_bytes=%zu last=%.0f (expect %d)\n", n, temp_bytes, last, n);

        const int iters = 200;
        double    best  = 1e30;
        for (int r = 0; r < 20; ++r)
        {
            cudaDeviceSynchronize();
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int it = 0; it < iters; ++it) { cub::DeviceScan::InclusiveSum(d_temp, temp_bytes, d_in, d_out, n); }
            cudaDeviceSynchronize();
            const auto   t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iters);
            if (ms < best) { best = ms; }
        }
        const double gbps = 2.0 * static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        std::printf("%-10d %-12.5f %-12.1f\n", n, best, gbps);

        cudaFree(d_in);
        cudaFree(d_out);
        cudaFree(d_temp);
    }
    return 0;
}
