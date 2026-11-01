// cub_reduce_bench.cu — B-cmp compute-primitive crush campaign: the VENDOR reduction baseline. CUB `DeviceReduce::Sum` is
// NVIDIA's production device-wide reduction (the gold every GPU reduce is measured against). A reduction is MEMORY-BOUND — it
// reads N elements once and emits one scalar — so the metric is achieved DRAM bandwidth (N·4 bytes / time). We compare our
// portable CKIR 2-pass reduction ([reduce-bench] in tests/gpu/gpu-context-vulkan) against this. N chosen > L2 so it is
// DRAM-bound. Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a wrong
// sum fails the run.

#include "cuda_bench.hpp"

#include <cub/device/device_reduce.cuh>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <memory>

int main()
{
    const int      sizes[] = {1 << 22, 1 << 24}; // 4.19M (16 MB) and 16.7M (64 MB) — both spill L2 ⇒ DRAM-bound
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# CUB DeviceReduce::Sum (f32) — %s\n", prop.name);
    std::printf("# %-10s %-12s %-12s\n", "N", "min_ms", "GB/s");

    for (const int n : sizes)
    {
        const size_t count = static_cast<size_t>(n);
        float*       d_in  = nullptr;
        float*       d_out = nullptr;
        CRD_CUDA_CHECK(cudaMalloc(&d_in, sizeof(float) * count));
        CRD_CUDA_CHECK(cudaMalloc(&d_out, sizeof(float)));
        // d_in = 1.0f, so the sum is a KNOWN value (= n) — verifies the reduce actually ran.
        {
            const auto ones = std::make_unique<float[]>(count);
            std::fill(ones.get(), ones.get() + count, 1.0f);
            CRD_CUDA_CHECK(cudaMemcpy(d_in, ones.get(), sizeof(float) * count, cudaMemcpyHostToDevice));
        }

        void*  d_temp     = nullptr;
        size_t temp_bytes = 0;
        CRD_CUDA_CHECK(cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n));
        CRD_CUDA_CHECK(cudaMalloc(&d_temp, temp_bytes));
        CRD_CUDA_CHECK(cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n));
        float h_out = -1.0f;
        CRD_CUDA_CHECK(cudaMemcpy(&h_out, d_out, sizeof(float), cudaMemcpyDeviceToHost));
        std::printf("# N=%d temp_bytes=%zu result=%.1f (expect %d)\n", n, temp_bytes, static_cast<double>(h_out), n);
        if (h_out != static_cast<float>(n)) // n <= 2^24: every partial sum of ones is exact in f32
        {
            std::fprintf(stderr, "CUB's sum of %d ones is %.1f\n", n, static_cast<double>(h_out));
            return 1;
        }

        const auto reduce = [&]
        {
            CRD_CUDA_CHECK(cub::DeviceReduce::Sum(d_temp, temp_bytes, d_in, d_out, n));
        };
        for (int i = 0; i < 5; ++i) // warmup
        {
            reduce();
        }
        // wall-clock over a batch (device-synced) ⇒ robust vs per-call event races
        const double best = crd_bench::best_wall_ms(20, 200, reduce);
        const double gbps = static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        std::printf("%-10d %-12.5f %-12.1f\n", n, best, gbps);

        CRD_CUDA_CHECK(cudaFree(d_in));
        CRD_CUDA_CHECK(cudaFree(d_out));
        CRD_CUDA_CHECK(cudaFree(d_temp));
    }
    return 0;
}
