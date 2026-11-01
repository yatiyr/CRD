// mlp_fused_bench.cu — the neural-radiance-cache forward duel: a fully fused 64-wide MLP (tensor cores, activations
// never leave the chip) against NVIDIA's strongest vendor pipeline (cuBLASLt fp16 GEMMs with fused ReLU epilogues,
// activations round-tripping DRAM between layers). The thesis (the FFT fusion doctrine): L layers of N x 64
// activations cost cuBLASLt ~2 L N 64 2 bytes of DRAM traffic; the fused kernel reads the input once and writes the
// output once (~2 N 64 2), about L times less. The network, its data and the vendor pipeline are mlp_bench.hpp's.
// Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a failed CUDA or
// cuBLASLt call, or a fused output that disagrees with cuBLASLt's, fails the run.

#include "mlp_bench.hpp"

#include <mma.h>

#include <cstdio>

using namespace crd_bench::mlp;

namespace
{
namespace wmma = nvcuda::wmma;

// One block takes kTile rows through every layer. Activations ping-pong in shared memory (128 x 64 fp16, 16 KiB each)
// and each layer's weights are staged in shared (8 KiB). Each warp computes a 64-row stripe as 4 x 4 wmma 16x16x16
// fragments (fp16 accumulation, a fixed schedule, so the result is deterministic); the ReLU runs on the accumulator
// fragments in registers. DRAM: the input read once, the output written once, the weights (48 KiB) from L2.
__global__ void __launch_bounds__(kWarps * 32)
    fused_mlp(const __half* __restrict__ in, const __half* __restrict__ w, __half* __restrict__ out)
{
    __shared__ __half sa[kTile * kWidth];
    __shared__ __half sb[kTile * kWidth];
    __shared__ __half sw[kWidth * kWidth];

    const int block_row = static_cast<int>(blockIdx.x) * kTile;
    const int tid       = static_cast<int>(threadIdx.x);
    const int warp      = tid / 32;

    const __half* src = in + static_cast<size_t>(block_row) * kWidth;
    for (int i = tid; i < kTile * kWidth / 8; i += kWarps * 32)
    {
        reinterpret_cast<float4*>(sa)[i] = reinterpret_cast<const float4*>(src)[i];
    }

    __half* cur = sa;
    __half* nxt = sb;
    for (int layer = 0; layer < kLayers; ++layer)
    {
        // this layer's weights, column-major 64 x 64 (w[k + 64 n] = W[n][k]): shared beats L2-direct fragment reads
        const __half* wl = w + kMatrix * layer;
        for (int i = tid; i < kWidth * kWidth / 8; i += kWarps * 32)
        {
            reinterpret_cast<float4*>(sw)[i] = reinterpret_cast<const float4*>(wl)[i];
        }
        __syncthreads(); // sw, and from layer 1 the previous layer's activations, visible before they are read

        // the warp's stripe as 4 row fragments x 4 column fragments: 16 independent accumulators
        wmma::fragment<wmma::accumulator, 16, 16, 16, __half> acc[4][4];
        for (int m = 0; m < 4; ++m)
        {
            for (int n = 0; n < 4; ++n)
            {
                wmma::fill_fragment(acc[m][n], __float2half(0.0F));
            }
        }
        for (int k = 0; k < 4; ++k)
        {
            wmma::fragment<wmma::matrix_a, 16, 16, 16, __half, wmma::row_major> af[4];
            for (int m = 0; m < 4; ++m)
            {
                wmma::load_matrix_sync(af[m], cur + (warp * 64 + m * 16) * kWidth + k * 16, kWidth);
            }
            for (int n = 0; n < 4; ++n)
            {
                wmma::fragment<wmma::matrix_b, 16, 16, 16, __half, wmma::row_major> bf;
                wmma::load_matrix_sync(bf, sw + (k * 16) * kWidth + n * 16, kWidth); // B[k][n] = w[64 k + n]
                for (int m = 0; m < 4; ++m)
                {
                    wmma::mma_sync(acc[m][n], af[m], bf, acc[m][n]);
                }
            }
        }
        if (layer + 1 < kLayers) // a hidden layer: the ReLU in registers
        {
            for (int m = 0; m < 4; ++m)
            {
                for (int n = 0; n < 4; ++n)
                {
                    for (int e = 0; e < acc[m][n].num_elements; ++e)
                    {
                        const __half value = acc[m][n].x[e];
                        acc[m][n].x[e]     = __hgt(value, __float2half(0.0F)) ? value : __float2half(0.0F);
                    }
                }
            }
        }
        // into the other buffer: cur is never written while it is read
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
        __syncthreads(); // the new activations visible, and sw free, before the next layer
    }

    __half* dst = out + static_cast<size_t>(block_row) * kWidth;
    for (int i = tid; i < kTile * kWidth / 8; i += kWarps * 32)
    {
        reinterpret_cast<float4*>(dst)[i] = reinterpret_cast<const float4*>(cur)[i];
    }
}
} // namespace

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# FUSED MLP (wmma fp16, activations on-chip) vs cublasLt fp16+ReLU-epilogue — %s\n", prop.name);
    std::printf("# MLP 64 x %d layers, batch %d\n", kLayers, kBatch);

    Lcg        lcg(12345U);
    const auto h_in  = random_halves(lcg, kActivations, 1.0F);
    const auto h_w   = random_halves(lcg, kWeights, 0.17F);
    const auto d_in  = upload(h_in.get(), kActivations);
    const auto d_w   = upload(h_w.get(), kWeights);
    const auto d_out = crd_bench::device_array<__half>(kActivations);
    const auto d_a   = crd_bench::device_array<__half>(kActivations);
    const auto d_b   = crd_bench::device_array<__half>(kActivations);

    const LtForward lt;
    const auto      cublas_mlp = [&]
    {
        lt.network(d_w.get(), d_in.get(), d_a.get(), d_b.get(), d_out.get());
    };
    const auto fused = [&]
    {
        fused_mlp<<<kBatch / kTile, kWarps * 32>>>(d_in.get(), d_w.get(), d_out.get());
        CRD_CUDA_CHECK(cudaGetLastError());
    };

    cublas_mlp();
    const auto reference = download(d_out.get(), kActivations);
    fused();
    const auto candidate = download(d_out.get(), kActivations);
    if (!outputs_agree("fused vs cuBLASLt", reference.get(), candidate.get()))
    {
        return 1;
    }

    const double flops  = 2.0 * kBatch * kWidth * kWidth * kLayers;
    const double cublas = time_row("cublasLt-MLP", flops, cublas_mlp);
    const double fast   = time_row("FUSED-MLP", flops, fused);
    std::printf("# RATIO: fused is %.2fx vs cublasLt\n", cublas / fast);
    return 0;
}
