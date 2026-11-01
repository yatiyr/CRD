// cufft_1024_profilee.cu — a minimal target for Nsight Compute: ONE 1024x1024 C2C 2-D convolution (fwd + multiply + inv)
// after one warmup, so `ncu` captures cuFFT's exact kernel anatomy (how many kernels, their grids, occupancy, L2/DRAM
// throughput) for the size we are chasing. Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by
// scripts/run_bench_gpu_cuda.sh (or under `ncu`).
#include "cuda_bench.hpp"
#include "spectrum_multiply.cuh"

#include <cuda_runtime.h>
#include <cufft.h>

#include <cstdio>

int main()
{
    const int       n     = 1024;
    const long long tot   = static_cast<long long>(n) * n;
    const size_t    bytes = sizeof(cufftComplex) * static_cast<size_t>(tot);
    cufftComplex*   d     = nullptr;
    cufftComplex*   f     = nullptr;
    CRD_CUDA_CHECK(cudaMalloc(&d, bytes));
    CRD_CUDA_CHECK(cudaMalloc(&f, bytes));
    CRD_CUDA_CHECK(cudaMemset(d, 0, bytes));
    CRD_CUDA_CHECK(cudaMemset(f, 0, bytes));
    cufftHandle plan;
    CRD_CUDA_CHECK(cufftPlan2d(&plan, n, n, CUFFT_C2C));
    // one warmup + one measured conv = ncu sees each kernel exactly twice; profile the 2nd instance.
    for (int r = 0; r < 2; ++r)
    {
        CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_FORWARD));
        crd_bench::multiply_spectrum(d, f, tot);
        CRD_CUDA_CHECK(cufftExecC2C(plan, d, d, CUFFT_INVERSE));
    }
    CRD_CUDA_CHECK(cudaDeviceSynchronize());
    std::printf("done\n");
    CRD_CUDA_CHECK(cufftDestroy(plan));
    CRD_CUDA_CHECK(cudaFree(d));
    CRD_CUDA_CHECK(cudaFree(f));
    return 0;
}
