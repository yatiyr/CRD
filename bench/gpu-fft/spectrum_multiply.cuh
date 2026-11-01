// spectrum_multiply.cuh — the middle pass of the cuFFT convolution baselines (forward FFT, this complex multiply,
// inverse FFT): the vendor path's separate elementwise kernel, one thread per complex element in blocks of 256.
#pragma once

#include "cuda_bench.hpp"

#include <cuda_runtime.h>

namespace crd_bench
{
// a[i] *= f[i] over `total` complex elements.
__global__ void spectrum_multiply(float2* a, const float2* f, long long total)
{
    const long long i = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < total)
    {
        const float2 x = a[i];
        const float2 y = f[i];
        a[i].x         = x.x * y.x - x.y * y.y;
        a[i].y         = x.x * y.y + x.y * y.x;
    }
}

// a[i] *= f[i % period]: one spectrum of `period` complex elements broadcast over a batch of `total / period` images.
__global__ void spectrum_multiply_broadcast(float2* a, const float2* f, long long period, long long total)
{
    const long long i = static_cast<long long>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (i < total)
    {
        const float2 x = a[i];
        const float2 y = f[i % period];
        a[i].x         = x.x * y.x - x.y * y.y;
        a[i].y         = x.x * y.y + x.y * y.x;
    }
}

constexpr int kSpectrumBlock = 256;

inline unsigned spectrum_grid(long long total)
{
    return static_cast<unsigned>((total + kSpectrumBlock - 1) / kSpectrumBlock);
}

inline void multiply_spectrum(float2* a, const float2* f, long long total)
{
    spectrum_multiply<<<spectrum_grid(total), kSpectrumBlock>>>(a, f, total);
    CRD_CUDA_CHECK(cudaGetLastError());
}

inline void multiply_spectrum_broadcast(float2* a, const float2* f, long long period, long long total)
{
    spectrum_multiply_broadcast<<<spectrum_grid(total), kSpectrumBlock>>>(a, f, period, total);
    CRD_CUDA_CHECK(cudaGetLastError());
}
} // namespace crd_bench
