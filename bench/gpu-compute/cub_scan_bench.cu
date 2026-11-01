// cub_scan_bench.cu — B-cmp: the VENDOR prefix-sum baseline. CUB `DeviceScan::InclusiveSum` is NVIDIA's production scan — a
// SINGLE-PASS decoupled-look-back kernel (reads N once, writes N once ≈ 2N traffic, using device-scope atomics + spin for the
// inter-block prefix). Our PORTABLE 3-pass scan (no atomics ⇒ deterministic + bit-exact on Vulkan/DX12) pays ~4N traffic, so
// this is the honest bar for a portable scan. Memory-bound ⇒ the metric is bandwidth. Built by CRD_BUILD_GPU_VS_CUDA
// (bench/CMakeLists.txt) for sm_89 SASS (the driver rejected 13.3 PTX JIT) and run by scripts/run_bench_gpu_cuda.sh; a
// wrong final prefix fails the run.

#include "cuda_bench.hpp"

#include <cub/device/device_scan.cuh>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <memory>

int main()
{
    const int      sizes[] = {1 << 22, 1 << 24}; // 16 MB (L2) · 64 MB (DRAM-bound)
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# CUB DeviceScan::InclusiveSum (f32) — %s\n", prop.name);
    std::printf("# %-10s %-12s %-12s\n", "N", "min_ms", "GB/s(2N)");

    for (const int n : sizes)
    {
        const size_t count = static_cast<size_t>(n);
        float*       d_in  = nullptr;
        float*       d_out = nullptr;
        CRD_CUDA_CHECK(cudaMalloc(&d_in, sizeof(float) * count));
        CRD_CUDA_CHECK(cudaMalloc(&d_out, sizeof(float) * count));
        {
            const auto ones = std::make_unique<float[]>(count);
            std::fill(ones.get(), ones.get() + count, 1.0f);
            CRD_CUDA_CHECK(cudaMemcpy(d_in, ones.get(), sizeof(float) * count, cudaMemcpyHostToDevice));
        }

        void*  d_temp     = nullptr;
        size_t temp_bytes = 0;
        CRD_CUDA_CHECK(cub::DeviceScan::InclusiveSum(d_temp, temp_bytes, d_in, d_out, n));
        CRD_CUDA_CHECK(cudaMalloc(&d_temp, temp_bytes));
        CRD_CUDA_CHECK(cub::DeviceScan::InclusiveSum(d_temp, temp_bytes, d_in, d_out, n));
        float last = 0.0f;
        CRD_CUDA_CHECK(cudaMemcpy(&last, d_out + (n - 1), sizeof(float), cudaMemcpyDeviceToHost));
        std::printf("# N=%d temp_bytes=%zu last=%.0f (expect %d)\n", n, temp_bytes, static_cast<double>(last), n);
        if (last != static_cast<float>(n)) // n <= 2^24: every prefix of ones is exact in f32
        {
            std::fprintf(stderr, "CUB's inclusive sum of %d ones ends at %.1f\n", n, static_cast<double>(last));
            return 1;
        }

        const double best = crd_bench::best_wall_ms(20, 200, [&]
        {
            CRD_CUDA_CHECK(cub::DeviceScan::InclusiveSum(d_temp, temp_bytes, d_in, d_out, n));
        });
        const double gbps = 2.0 * static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        std::printf("%-10d %-12.5f %-12.1f\n", n, best, gbps);

        CRD_CUDA_CHECK(cudaFree(d_in));
        CRD_CUDA_CHECK(cudaFree(d_out));
        CRD_CUDA_CHECK(cudaFree(d_temp));
    }
    return 0;
}
