// ckir_mlp_bench.cu — the CKIR-authored fused MLP forward against the same cuBLASLt pipeline mlp_fused_bench.cu
// duels. The kernel is emitted by CKIR into ckir_mlp_fwd_gen.cu (test_ckir_mlp.cpp, the hidden [.emit-cuda-mlp] case),
// not written here; the network, its data and the vendor pipeline are mlp_bench.hpp's. Expect the hand-written
// kernel's ~2.37x. Built by CRD_BUILD_GPU_VS_CUDA (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh; a
// failed CUDA or cuBLASLt call, an output that disagrees with cuBLASLt's or two runs that differ fail the run.

#include "ckir_mlp_fwd_gen.cu" // ckir_fused_mlp_fwd(in, w, out, nrows): width 64, 6 layers, tile 128, 2 warps
#include "mlp_bench.hpp"

#include <cstdio>

using namespace crd_bench::mlp;

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# CKIR-AUTHORED fused MLP (generated wmma) vs cublasLt — %s\n", prop.name);
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
    const auto ckir = [&]
    {
        ckir_fused_mlp_fwd<<<kBatch / kTile, kWarps * 32>>>(d_in.get(), d_w.get(), d_out.get(), kBatch);
        CRD_CUDA_CHECK(cudaGetLastError());
    };

    cublas_mlp();
    const auto reference = download(d_out.get(), kActivations);
    ckir();
    const auto first = download(d_out.get(), kActivations);
    if (!outputs_agree("CKIR vs cuBLASLt", reference.get(), first.get()))
    {
        return 1;
    }

    // a fixed schedule without atomics: a second run is bit-identical
    ckir();
    const auto second = download(d_out.get(), kActivations);
    size_t     differing = 0;
    for (size_t i = 0; i < kActivations; ++i)
    {
        if (static_cast<__half_raw>(first[i]).x != static_cast<__half_raw>(second[i]).x)
        {
            ++differing;
        }
    }
    std::printf("# determinism (run-to-run): %zu/%zu differing halves\n", differing, kActivations);
    if (differing != 0)
    {
        std::fprintf(stderr, "two runs of the CKIR kernel differ in %zu halves\n", differing);
        return 1;
    }

    const double flops  = 2.0 * kBatch * kWidth * kWidth * kLayers;
    const double cublas = time_row("cublasLt-MLP", flops, cublas_mlp);
    const double fast   = time_row("CKIR-fused-MLP", flops, ckir);
    std::printf("# RATIO: CKIR-authored fused is %.2fx vs cublasLt\n", cublas / fast);
    return 0;
}
