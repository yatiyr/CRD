// mlp_backward_bench.cu — the training half of the neural-radiance-cache duel: a fully fused MLP backward pass
// (activation gradients never leave the chip) against NVIDIA's vendor backward (cuBLASLt: per layer a ReLU-mask pass,
// a da GEMM and a dW GEMM, each round-tripping DRAM). The thesis is the forward's: cuBLASLt writes every dz and da to
// DRAM between calls, the fused kernel walks the layers backward with the gradient tile resident in shared memory.
// The network, the backward's equations, the data and the vendor pipelines are mlp_bench.hpp's. Built by
// CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a failed CUDA or cuBLASLt
// call, or a dW[L-1] from either side that disagrees with the CPU oracle, fails the run.

#include "mlp_bench.hpp"

#include <mma.h>

#include <cstdio>

using namespace crd_bench::mlp;

namespace
{
namespace wmma = nvcuda::wmma;

// The kGroups partial dW copies summed into the final dW.
__global__ void reduce_dw(const float* __restrict__ partial, float* __restrict__ dw)
{
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < static_cast<int>(kWeights))
    {
        float acc = 0.0F;
        for (int group = 0; group < kGroups; ++group)
        {
            acc += partial[kWeights * group + i];
        }
        dw[i] = acc;
    }
}

// One block takes kTile rows backward through every layer: dz = g (.) act'(z) in place, dW[layer] += a[layer]^T . dz
// on the tensor cores into the block's partial copy (fp32 atomics), and da = dz . W^T into the other gradient buffer.
__global__ void __launch_bounds__(kWarps * 32)
    fused_mlp_bwd(const __half* __restrict__ acts, const __half* __restrict__ w, const __half* __restrict__ gout,
                  float* __restrict__ dw)
{
    __shared__ __half sg_a[kTile * kWidth];
    __shared__ __half sg_b[kTile * kWidth];
    __shared__ __half sw[kWidth * kWidth];
    __shared__ float  sdw[kWarps][16 * 16]; // each warp stores its dW fragment here before the atomic scatter

    const int block_row = static_cast<int>(blockIdx.x) * kTile;
    const int tid       = static_cast<int>(threadIdx.x);
    const int warp      = tid / 32;

    const __half* src = gout + static_cast<size_t>(block_row) * kWidth;
    for (int i = tid; i < kTile * kWidth / 8; i += kWarps * 32)
    {
        reinterpret_cast<float4*>(sg_a)[i] = reinterpret_cast<const float4*>(src)[i];
    }
    __half* cur = sg_a;
    __half* nxt = sg_b;
    __syncthreads();

    for (int layer = kLayers - 1; layer >= 0; --layer)
    {
        const __half* wl    = w + kMatrix * layer;
        const __half* a_in  = acts + kActivations * layer + static_cast<size_t>(block_row) * kWidth;
        const __half* a_out = acts + kActivations * (layer + 1) + static_cast<size_t>(block_row) * kWidth;
        for (int i = tid; i < kWidth * kWidth / 8; i += kWarps * 32)
        {
            reinterpret_cast<float4*>(sw)[i] = reinterpret_cast<const float4*>(wl)[i];
        }
        __syncthreads();

        if (layer + 1 < kLayers) // dz = g (.) act'(z) in place; the output layer is linear
        {
            for (int i = tid; i < kTile * kWidth; i += kWarps * 32)
            {
                cur[i] = __hgt(a_out[i], __float2half(0.0F)) ? cur[i] : __float2half(0.0F);
            }
            __syncthreads();
        }

        // dW[layer] += a[layer]^T . dz (64 x 64 fp32): the warp owns dW columns [warp 32, warp 32 + 32), as 2 column x
        // 4 row fragments contracted over the tile's rows
        wmma::fragment<wmma::accumulator, 16, 16, 16, float> dacc[2][4];
        for (int cb = 0; cb < 2; ++cb)
        {
            for (int rb = 0; rb < 4; ++rb)
            {
                wmma::fill_fragment(dacc[cb][rb], 0.0F);
            }
        }
        for (int kb = 0; kb < kTile / 16; ++kb)
        {
            wmma::fragment<wmma::matrix_a, 16, 16, 16, __half, wmma::col_major> af[4]; // a^T: dW row k x batch
            for (int rb = 0; rb < 4; ++rb)
            {
                wmma::load_matrix_sync(af[rb], a_in + (kb * 16) * kWidth + rb * 16, kWidth);
            }
            for (int cb = 0; cb < 2; ++cb)
            {
                wmma::fragment<wmma::matrix_b, 16, 16, 16, __half, wmma::row_major> bf; // dz: batch x dW column n
                wmma::load_matrix_sync(bf, cur + (kb * 16) * kWidth + (warp * 32 + cb * 16), kWidth);
                for (int rb = 0; rb < 4; ++rb)
                {
                    wmma::mma_sync(dacc[cb][rb], af[rb], bf, dacc[cb][rb]);
                }
            }
        }
        float* dwl = dw + (static_cast<size_t>(blockIdx.x % kGroups) * kLayers + layer) * kMatrix;
        for (int cb = 0; cb < 2; ++cb)
        {
            for (int rb = 0; rb < 4; ++rb)
            {
                wmma::store_matrix_sync(sdw[warp], dacc[cb][rb], 16, wmma::mem_row_major); // sdw[row k, column n]
                __syncwarp();
                for (int e = (tid & 31); e < 256; e += 32)
                {
                    const int kk = rb * 16 + e / 16;             // dW row k
                    const int nn = warp * 32 + cb * 16 + e % 16; // dW column n
                    atomicAdd(&dwl[kk + kWidth * nn], sdw[warp][e]);
                }
                __syncwarp();
            }
        }
        __syncthreads();

        // da = dz . W^T, the next g: da[r,k] = sum_n dz[r,n] sw[n 64 + k], A = dz and B[n,k] both row-major
        wmma::fragment<wmma::accumulator, 16, 16, 16, __half> acc[4][4];
        for (int m = 0; m < 4; ++m)
        {
            for (int n = 0; n < 4; ++n)
            {
                wmma::fill_fragment(acc[m][n], __float2half(0.0F));
            }
        }
        for (int kb = 0; kb < 4; ++kb)
        {
            wmma::fragment<wmma::matrix_a, 16, 16, 16, __half, wmma::row_major> af[4];
            for (int m = 0; m < 4; ++m)
            {
                wmma::load_matrix_sync(af[m], cur + (warp * 64 + m * 16) * kWidth + kb * 16, kWidth);
            }
            for (int n = 0; n < 4; ++n)
            {
                wmma::fragment<wmma::matrix_b, 16, 16, 16, __half, wmma::row_major> bf;
                wmma::load_matrix_sync(bf, sw + (kb * 16) * kWidth + n * 16, kWidth); // B[n,k] = sw[n 64 + k]
                for (int m = 0; m < 4; ++m)
                {
                    wmma::mma_sync(acc[m][n], af[m], bf, acc[m][n]);
                }
            }
        }
        for (int m = 0; m < 4; ++m)
        {
            for (int n = 0; n < 4; ++n)
            {
                wmma::store_matrix_sync(nxt + (warp * 64 + m * 16) * kWidth + n * 16, acc[m][n], kWidth,
                                        wmma::mem_row_major);
            }
        }
        __half* const done = cur;
        cur                = nxt;
        nxt                = done;
        __syncthreads();
    }
}
} // namespace

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# FUSED MLP BACKWARD (wmma, on-chip gradient chain) vs cublasLt backward pipeline — %s\n", prop.name);
    std::printf("# MLP 64 x %d layers, batch %d\n", kLayers, kBatch);

    Lcg        lcg(999983U);
    const auto h_w       = random_halves(lcg, kWeights, 0.17F);
    const auto h_in      = random_halves(lcg, kActivations, 1.0F);
    const auto d_w       = upload(h_w.get(), kWeights);
    const auto d_acts    = crd_bench::device_array<__half>(kActivations * (kLayers + 1));
    const auto d_gout    = crd_bench::device_array<__half>(kActivations);
    const auto d_dz      = crd_bench::device_array<__half>(kActivations);
    const auto d_da      = crd_bench::device_array<__half>(kActivations);
    const auto d_dw      = crd_bench::device_array<float>(kWeights);
    const auto d_dw_ref  = crd_bench::device_array<float>(kWeights);
    const auto d_dw_part = crd_bench::device_array<float>(kWeights * kGroups); // spreads the atomics' contention
    CRD_CUDA_CHECK(cudaMemcpy(d_acts.get(), h_in.get(), sizeof(__half) * kActivations, cudaMemcpyHostToDevice));

    // the forward through cuBLASLt keeps every activation; the loss's gradient is the output itself
    const LtForward lt;
    lt.network_keeping_activations(d_w.get(), d_acts.get());
    CRD_CUDA_CHECK(cudaMemcpy(d_gout.get(), d_acts.get() + kActivations * kLayers, sizeof(__half) * kActivations,
                              cudaMemcpyDeviceToDevice));
    const auto oracle = last_layer_weight_gradient(d_acts.get(), d_gout.get());

    const auto fused = [&]
    {
        CRD_CUDA_CHECK(cudaMemset(d_dw_part.get(), 0, sizeof(float) * kWeights * kGroups));
        fused_mlp_bwd<<<kBatch / kTile, kWarps * 32>>>(d_acts.get(), d_w.get(), d_gout.get(), d_dw_part.get());
        CRD_CUDA_CHECK(cudaGetLastError());
        reduce_dw<<<static_cast<unsigned>((kWeights + 255) / 256), 256>>>(d_dw_part.get(), d_dw.get());
        CRD_CUDA_CHECK(cudaGetLastError());
    };
    fused();
    if (!weight_gradient_agrees("FUSED", d_dw.get(), oracle.get()))
    {
        return 1;
    }

    const LtBackward backward(lt);
    const auto       cublas_bwd = [&]
    {
        backward.network(d_w.get(), d_acts.get(), d_gout.get(), d_dz.get(), d_da.get(), d_dw_ref.get());
    };
    cublas_bwd();
    if (!weight_gradient_agrees("cuBLAS", d_dw_ref.get(), oracle.get()))
    {
        return 1;
    }

    time_backward_parts(backward, d_w.get(), d_acts.get(), d_gout.get(), d_dz.get(), d_da.get(), d_dw_ref.get());
    const double flops  = 4.0 * kBatch * kWidth * kWidth * kLayers; // dW and da: two GEMMs per layer
    const double cublas = time_row("cublasLt-bwd", flops, cublas_bwd);
    const double fast   = time_row("FUSED-bwd", flops, fused);
    std::printf("# RATIO: fused is %.2fx vs cublasLt (backward)\n", cublas / fast);
    return 0;
}
