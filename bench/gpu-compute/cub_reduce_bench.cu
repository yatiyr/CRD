// cub_reduce_bench.cu — B-cmp compute-primitive crush campaign: the VENDOR reduction baseline. CUB `DeviceReduce::Sum` is
// NVIDIA's production device-wide reduction (the gold every GPU reduce is measured against). A reduction is MEMORY-BOUND — it
// reads N elements once and emits one scalar — so the metric is achieved DRAM bandwidth (N·4 bytes / time). We compare our
// portable CKIR 2-pass reduction ([reduce-bench] in tests/gpu-context-vulkan) against this. N chosen > L2 so it is DRAM-bound.
// Build: nvcc -O3 -allow-unsupported-compiler -I"...\include\cccl" cub_reduce_bench.cu -o cub_reduce_bench.exe

#include <cuda_runtime.h>
#include <cub/device/device_reduce.cuh>

#include <chrono>
#include <cstdio>

int main()
{
    const int      sizes[] = {1 << 22, 1 << 24}; // 4.19M (16 MB) and 16.7M (64 MB) — both spill L2 ⇒ DRAM-bound
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# CUB DeviceReduce::Sum (f32) — %s\n", prop.name);
    std::printf("# %-10s %-12s %-12s\n", "N", "min_ms", "GB/s");

    for (int si = 0; si < 2; ++si)
    {
        const int n = sizes[si];
        float*    d_in  = nullptr;
        float*    d_out = nullptr;
        cudaMalloc(&d_in, sizeof(float) * static_cast<size_t>(n));
        cudaMalloc(&d_out, sizeof(float));
        // fill d_in = 1.0f (host → device) so the sum is a KNOWN value (= n) — verifies the reduce actually ran.
        {
            float* h = static_cast<float*>(malloc(sizeof(float) * static_cast<size_t>(n)));
            for (int i = 0; i < n; ++i) { h[i] = 1.0f; }
            cudaMemcpy(d_in, h, sizeof(float) * static_cast<size_t>(n), cudaMemcpyHostToDevice);
            free(h);
        }

        void*       d_temp     = nullptr;
        size_t      temp_bytes = 0;
        cudaError_t qerr        = cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n);
        if (qerr != cudaSuccess) { std::printf("# query error N=%d: %s\n", n, cudaGetErrorString(qerr)); }
        cudaMalloc(&d_temp, temp_bytes);
        cudaError_t rerr = cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n);
        cudaDeviceSynchronize();
        float h_out = -1.0f;
        cudaMemcpy(&h_out, d_out, sizeof(float), cudaMemcpyDeviceToHost);
        std::printf("# N=%d temp_bytes=%zu reduce_err=%s result=%.1f (expect %d)\n", n, temp_bytes, cudaGetErrorString(rerr), h_out, n);

        for (int i = 0; i < 5; ++i) { cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n); }
        cudaError_t werr = cudaDeviceSynchronize();
        if (werr != cudaSuccess) { std::printf("# warmup error N=%d: %s\n", n, cudaGetErrorString(werr)); }

        const int iters = 200; // wall-clock over a batch (device-synced) ⇒ robust vs per-call event races
        double    best  = 1e30;
        for (int r = 0; r < 20; ++r)
        {
            cudaDeviceSynchronize();
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int it = 0; it < iters; ++it) { cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n); }
            cudaDeviceSynchronize();
            const auto   t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iters);
            if (ms < best) { best = ms; }
        }
        const double gbps = static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        std::printf("%-10d %-12.5f %-12.1f\n", n, best, gbps);

        cudaFree(d_in);
        cudaFree(d_out);
        cudaFree(d_temp);
    }
    return 0;
}
