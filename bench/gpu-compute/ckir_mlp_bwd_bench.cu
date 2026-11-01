// ckir_mlp_bwd_bench.cu — the CKIR-authored fused MLP backward against the same cuBLASLt backward
// mlp_backward_bench.cu duels. Its kernels (reduce_dw and ckir_fused_mlp_bwd) are emitted by CKIR into
// ckir_mlp_bwd_gen.cu (test_ckir_mlp.cpp, the hidden [.emit-cuda-mlp-bwd] case), not written here; the network, the
// backward's equations, the data and the vendor pipelines are mlp_bench.hpp's. Expect ~1.9x. Built by
// CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a failed CUDA or cuBLASLt
// call, or a dW[L-1] from either side that disagrees with the CPU oracle, fails the run.

#include "ckir_mlp_bwd_gen.cu" // reduce_dw and ckir_fused_mlp_bwd: width 64, 6 layers, tile 128, 2 warps, 8 groups
#include "mlp_bench.hpp"

#include <cstdio>

using namespace crd_bench::mlp;

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# CKIR-AUTHORED tensor backward (generated wmma) vs cublasLt backward pipeline — %s\n", prop.name);
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
        ckir_fused_mlp_bwd<<<kBatch / kTile, kWarps * 32>>>(d_acts.get(), d_w.get(), d_gout.get(), d_dw_part.get());
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
