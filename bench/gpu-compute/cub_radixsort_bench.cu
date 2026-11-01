// cub_radixsort_bench.cu — B-cmp: the VENDOR sort baseline. CUB `DeviceRadixSort::SortKeys` (u32) is NVIDIA's production sort.
// A sort is memory-bound (each pass reads + writes N keys); CUB uses 8-bit digits (4 passes for 32-bit) with a warp-level
// rank. Our portable CKIR radix sort is bit-exact + stable; the crush target is here. Built by CRD_BUILD_GPU_VS_CUDA
// (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a key sequence CUB leaves unsorted fails the run.

#include "cuda_bench.hpp"

#include <cub/device/device_radix_sort.cuh>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <memory>

int main()
{
    const int      sizes[] = {1 << 20, 1 << 22, 1 << 24}; // 1M, 4M, 16.7M keys
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# CUB DeviceRadixSort::SortKeys (u32) — %s\n", prop.name);
    std::printf("# %-10s %-12s %-14s\n", "N", "min_ms", "Mkeys/s");

    for (const int n : sizes)
    {
        const size_t count = static_cast<size_t>(n);
        unsigned*    d_in  = nullptr;
        unsigned*    d_out = nullptr;
        CRD_CUDA_CHECK(cudaMalloc(&d_in, sizeof(unsigned) * count));
        CRD_CUDA_CHECK(cudaMalloc(&d_out, sizeof(unsigned) * count));
        const auto keys = std::make_unique<unsigned[]>(count);
        for (size_t i = 0; i < count; ++i)
        {
            keys[i] = (static_cast<unsigned>(i) * 1103515245U + 12345U) ^ (static_cast<unsigned>(i) << 13);
        }
        CRD_CUDA_CHECK(cudaMemcpy(d_in, keys.get(), sizeof(unsigned) * count, cudaMemcpyHostToDevice));

        void*  d_temp     = nullptr;
        size_t temp_bytes = 0;
        CRD_CUDA_CHECK(cub::DeviceRadixSort::SortKeys(d_temp, temp_bytes, d_in, d_out, n));
        CRD_CUDA_CHECK(cudaMalloc(&d_temp, temp_bytes));
        CRD_CUDA_CHECK(cub::DeviceRadixSort::SortKeys(d_temp, temp_bytes, d_in, d_out, n));
        CRD_CUDA_CHECK(cudaMemcpy(keys.get(), d_out, sizeof(unsigned) * count, cudaMemcpyDeviceToHost));
        if (!std::is_sorted(keys.get(), keys.get() + count))
        {
            std::fprintf(stderr, "CUB left the %d keys unsorted\n", n);
            return 1;
        }

        const double best = crd_bench::best_wall_ms(15, 50, [&]
        {
            CRD_CUDA_CHECK(cub::DeviceRadixSort::SortKeys(d_temp, temp_bytes, d_in, d_out, n));
        });
        std::printf("%-10d %-12.5f %-14.1f\n", n, best, static_cast<double>(n) / (best * 1.0e3));

        CRD_CUDA_CHECK(cudaFree(d_in));
        CRD_CUDA_CHECK(cudaFree(d_out));
        CRD_CUDA_CHECK(cudaFree(d_temp));
    }
    return 0;
}
