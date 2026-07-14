// cub_radixsort_bench.cu — B-cmp: the VENDOR sort baseline. CUB `DeviceRadixSort::SortKeys` (u32) is NVIDIA's production sort.
// A sort is memory-bound (each pass reads + writes N keys); CUB uses 8-bit digits (4 passes for 32-bit) with a warp-level
// rank. Our portable CKIR radix sort is bit-exact + stable; the crush target is here. Build: nvcc -O3 -std=c++17 -arch=sm_89
// -allow-unsupported-compiler -Xcompiler /Zc:preprocessor -I ...\include\cccl cub_radixsort_bench.cu -o ....exe

#include <cuda_runtime.h>
#include <cub/device/device_radix_sort.cuh>

#include <chrono>
#include <cstdio>

int main()
{
    const int      sizes[] = {1 << 20, 1 << 22, 1 << 24}; // 1M, 4M, 16.7M keys
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# CUB DeviceRadixSort::SortKeys (u32) — %s\n", prop.name);
    std::printf("# %-10s %-12s %-14s\n", "N", "min_ms", "Mkeys/s");

    for (int si = 0; si < 3; ++si)
    {
        const int n = sizes[si];
        unsigned* d_in  = nullptr;
        unsigned* d_out = nullptr;
        cudaMalloc(&d_in, sizeof(unsigned) * static_cast<size_t>(n));
        cudaMalloc(&d_out, sizeof(unsigned) * static_cast<size_t>(n));
        {
            unsigned* h = static_cast<unsigned*>(malloc(sizeof(unsigned) * static_cast<size_t>(n)));
            for (int i = 0; i < n; ++i) { h[i] = (static_cast<unsigned>(i) * 1103515245U + 12345U) ^ (static_cast<unsigned>(i) << 13); }
            cudaMemcpy(d_in, h, sizeof(unsigned) * static_cast<size_t>(n), cudaMemcpyHostToDevice);
            free(h);
        }

        void*  d_temp     = nullptr;
        size_t temp_bytes = 0;
        cub::DeviceRadixSort::SortKeys(d_temp, temp_bytes, d_in, d_out, n);
        cudaMalloc(&d_temp, temp_bytes);
        cub::DeviceRadixSort::SortKeys(d_temp, temp_bytes, d_in, d_out, n);
        cudaDeviceSynchronize();
        unsigned first = 1U, last = 0U;
        cudaMemcpy(&first, d_out, sizeof(unsigned), cudaMemcpyDeviceToHost);
        cudaMemcpy(&last, d_out + (n - 1), sizeof(unsigned), cudaMemcpyDeviceToHost);
        std::printf("# N=%d sorted? first=%u last=%u (first<=last: %d)\n", n, first, last, first <= last);

        const int iters = 50;
        double    best  = 1e30;
        for (int r = 0; r < 15; ++r)
        {
            cudaDeviceSynchronize();
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int it = 0; it < iters; ++it) { cub::DeviceRadixSort::SortKeys(d_temp, temp_bytes, d_in, d_out, n); }
            cudaDeviceSynchronize();
            const auto   t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(iters);
            if (ms < best) { best = ms; }
        }
        std::printf("%-10d %-12.5f %-14.1f\n", n, best, static_cast<double>(n) / (best * 1.0e3));

        cudaFree(d_in);
        cudaFree(d_out);
        cudaFree(d_temp);
    }
    return 0;
}
