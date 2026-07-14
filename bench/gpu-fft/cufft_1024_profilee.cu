// cufft_1024_profilee.cu — a minimal target for Nsight Compute: ONE 1024x1024 C2C 2-D convolution (fwd + multiply + inv)
// after one warmup, so `ncu` captures cuFFT's exact kernel anatomy (how many kernels, their grids, occupancy, L2/DRAM
// throughput) for the size we are chasing. Build: nvcc -O3 -allow-unsupported-compiler cufft_1024_profilee.cu -o
// cufft_1024_profilee.exe -lcufft
#include <cuda_runtime.h>
#include <cufft.h>
#include <cstdio>

__global__ void cmul(cufftComplex* a, const cufftComplex* b, long long total)
{
    long long i = (long long)blockIdx.x * blockDim.x + threadIdx.x;
    if (i < total)
    {
        cufftComplex x = a[i];
        cufftComplex y = b[i];
        a[i].x         = x.x * y.x - x.y * y.y;
        a[i].y         = x.x * y.y + x.y * y.x;
    }
}

int main()
{
    const int       n   = 1024;
    const long long tot = (long long)n * n;
    cufftComplex*   d   = nullptr;
    cufftComplex*   f   = nullptr;
    cudaMalloc(&d, sizeof(cufftComplex) * tot);
    cudaMalloc(&f, sizeof(cufftComplex) * tot);
    cudaMemset(d, 0, sizeof(cufftComplex) * tot);
    cudaMemset(f, 0, sizeof(cufftComplex) * tot);
    cufftHandle plan;
    cufftPlan2d(&plan, n, n, CUFFT_C2C);
    const int block = 256;
    const int grid  = (int)((tot + block - 1) / block);
    // one warmup + one measured conv = ncu sees each kernel exactly twice; profile the 2nd instance.
    for (int r = 0; r < 2; ++r)
    {
        cufftExecC2C(plan, d, d, CUFFT_FORWARD);
        cmul<<<grid, block>>>(d, f, tot);
        cufftExecC2C(plan, d, d, CUFFT_INVERSE);
    }
    cudaDeviceSynchronize();
    std::printf("done\n");
    cufftDestroy(plan);
    cudaFree(d);
    cudaFree(f);
    return 0;
}
