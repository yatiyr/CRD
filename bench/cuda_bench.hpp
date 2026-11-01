// cuda_bench.hpp — what the GPU boards' CUDA programs share (bench/CMakeLists.txt): one failure path, owned device
// arrays and two timers.
//
// CRD_CUDA_CHECK(call) runs a CUDA runtime, cuFFT or cuBLASLt call and, unless it returns its success status (0 in each
// library), prints the call, its file and line and the status, and exits 1, so a failed allocation, plan, launch or
// transfer fails the board run instead of timing nothing or printing a wrong row. A kernel launch is checked by
// CRD_CUDA_CHECK(cudaGetLastError()) after it; an asynchronous fault surfaces at the next checked synchronisation.
//
// device_array<T>(count) allocates `count` elements on the device (a failed allocation fails the run); the returned
// owner frees them.
//
// best_event_ms(runs, body) times `body` (work queued on the default stream) between two CUDA events `runs` times and
// returns the least GPU time in milliseconds: the peer's strongest number, the most conservative bar for Cerid's rows.
// best_wall_ms(runs, calls, body) times `calls` back-to-back calls of `body` on the host clock between device
// synchronisations, `runs` times, and returns the least time per call: robust against per-call event races for a
// library call that queues several kernels.
#pragma once

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <type_traits>

namespace crd_bench
{
template <typename Status>
void cuda_check(Status status, const char* call, const char* file, int line)
{
    static_assert(std::is_enum_v<Status>, "a CUDA, cuFFT or cuBLASLt status");
    if (status == Status{})
    {
        return;
    }
    const auto code = static_cast<int>(status);
    if constexpr (std::is_same_v<Status, cudaError_t>)
    {
        std::fprintf(stderr, "%s:%d: %s failed: %s (%d)\n", file, line, call, cudaGetErrorString(status), code);
    }
    else
    {
        std::fprintf(stderr, "%s:%d: %s failed with status %d\n", file, line, call, code);
    }
    std::exit(1);
}
} // namespace crd_bench

#define CRD_CUDA_CHECK(call) ::crd_bench::cuda_check((call), #call, __FILE__, __LINE__)

namespace crd_bench
{
struct DeviceFree
{
    void operator()(void* pointer) const
    {
        CRD_CUDA_CHECK(cudaFree(pointer));
    }
};

template <typename T>
using DeviceArray = std::unique_ptr<T[], DeviceFree>;

template <typename T>
DeviceArray<T> device_array(size_t count)
{
    T* pointer = nullptr;
    CRD_CUDA_CHECK(cudaMalloc(&pointer, sizeof(T) * count));
    return DeviceArray<T>(pointer);
}

template <typename Body>
float best_event_ms(int runs, Body&& body)
{
    cudaEvent_t start;
    cudaEvent_t stop;
    CRD_CUDA_CHECK(cudaEventCreate(&start));
    CRD_CUDA_CHECK(cudaEventCreate(&stop));
    float best = 1e30f;
    for (int run = 0; run < runs; ++run)
    {
        CRD_CUDA_CHECK(cudaEventRecord(start));
        body();
        CRD_CUDA_CHECK(cudaEventRecord(stop));
        CRD_CUDA_CHECK(cudaEventSynchronize(stop));
        float ms = 0.0f;
        CRD_CUDA_CHECK(cudaEventElapsedTime(&ms, start, stop));
        if (ms < best)
        {
            best = ms;
        }
    }
    CRD_CUDA_CHECK(cudaEventDestroy(start));
    CRD_CUDA_CHECK(cudaEventDestroy(stop));
    return best;
}

template <typename Body>
double best_wall_ms(int runs, int calls, Body&& body)
{
    double best = 1e30;
    for (int run = 0; run < runs; ++run)
    {
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const auto start = std::chrono::steady_clock::now();
        for (int call = 0; call < calls; ++call)
        {
            body();
        }
        CRD_CUDA_CHECK(cudaDeviceSynchronize());
        const auto   stop    = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double, std::milli>(stop - start).count();
        const double ms      = elapsed / static_cast<double>(calls);
        if (ms < best)
        {
            best = ms;
        }
    }
    return best;
}
} // namespace crd_bench
