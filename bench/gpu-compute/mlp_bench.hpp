// mlp_bench.hpp — what the fused-MLP boards' four CUDA programs share (mlp_fused_bench and mlp_backward_bench with
// their hand-written kernels, ckir_mlp_bench and ckir_mlp_bwd_bench with the CKIR-emitted ones;
// docs/bench/2026-07-14-fused-mlp-cublas-gold.md): the network, its deterministic weights and inputs, the cuBLASLt
// forward and backward pipelines the fused kernels duel, the CPU oracle of the last layer's weight gradient, the
// checks that judge a result and the timer that prints a row.
//
// The network: input 64 -> 5 hidden 64 (ReLU) -> 64 linear out, fp16 with fp16 accumulation, a batch of 2^20 rows.
// Layer l's weights are stored w[l][k + 64 * n] = W[l][k][n] (column-major for cuBLASLt). The backward pass takes
// the loss 1/2 ||out||^2, so dL/dout = out, and per layer l (a[l + 1] = act(z[l]), z[l] = a[l] . W[l]):
//   dz[l]      = g (.) act'(z[l])          act' = 1 on the linear output layer, (a[l + 1] > 0) on a ReLU layer
//   dW[l][k,n] = sum_r a[l][r,k] dz[l][r,n] reduced over the batch in fp32
//   da[l][r,k] = sum_n dz[l][r,n] W[l][k,n] the next layer's g
// Every failed CUDA or cuBLASLt call ends the run through CRD_CUDA_CHECK (cuda_bench.hpp).
#pragma once

#include "cuda_bench.hpp"

#include <cublasLt.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace crd_bench::mlp
{
constexpr int    kWidth       = 64;      // every layer is 64 wide
constexpr int    kLayers      = 6;       // weight matrices: 5 hidden ReLU layers and the linear output
constexpr int    kBatch       = 1 << 20; // rows through the network
constexpr int    kTile        = 128;     // batch rows per block of a fused kernel
constexpr int    kWarps       = 2;       // warps per block; each owns 64 rows
constexpr int    kGroups      = 8;       // partial dW copies the fused backward spreads its atomics over
constexpr size_t kMatrix      = static_cast<size_t>(kWidth) * kWidth;
constexpr size_t kWeights     = kMatrix * kLayers;
constexpr size_t kActivations = static_cast<size_t>(kBatch) * kWidth;

// The boards' generator: a 32-bit LCG whose top 23 bits give a value in [-1, 1).
class Lcg
{
public:
    explicit Lcg(unsigned seed) : state_(seed)
    {
    }

    float next()
    {
        state_ = state_ * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<int>(state_ >> 9) - (1 << 22)) / static_cast<float>(1 << 22);
    }

private:
    unsigned state_;
};

// `count` halves drawn from `lcg` and scaled; weights take 0.17, which keeps six fp16 layers' activations in range.
inline std::unique_ptr<__half[]> random_halves(Lcg& lcg, size_t count, float scale)
{
    auto values = std::make_unique<__half[]>(count);
    for (size_t i = 0; i < count; ++i)
    {
        values[i] = __float2half(lcg.next() * scale);
    }
    return values;
}

template <typename T>
DeviceArray<T> upload(const T* host, size_t count)
{
    auto device = device_array<T>(count);
    CRD_CUDA_CHECK(cudaMemcpy(device.get(), host, sizeof(T) * count, cudaMemcpyHostToDevice));
    return device;
}

template <typename T>
std::unique_ptr<T[]> download(const T* device, size_t count)
{
    auto host = std::make_unique<T[]>(count);
    CRD_CUDA_CHECK(cudaMemcpy(host.get(), device, sizeof(T) * count, cudaMemcpyDeviceToHost));
    return host;
}

// dz = g (.) (a_out > 0) on a ReLU layer, dz = g on the linear one: the cuBLASLt backward's separate mask pass.
extern "C" __global__ void relu_mask(const __half* __restrict__ g, const __half* __restrict__ a_out,
                                     __half* __restrict__ dz, int n, int do_mask)
{
    const int i = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n)
    {
        dz[i] = (do_mask != 0 && !__hgt(a_out[i], __float2half(0.0F))) ? __float2half(0.0F) : g[i];
    }
}

// The vendor forward: one cuBLASLt fp16 GEMM per layer, C(64, N) = W(64, 64) x A(64, N) column-major, with the ReLU
// epilogue fused on the hidden layers; activations round-trip DRAM between layers.
class LtForward
{
public:
    LtForward()
    {
        CRD_CUDA_CHECK(cublasLtCreate(&handle_));
        CRD_CUDA_CHECK(cublasLtMatmulDescCreate(&relu_, CUBLAS_COMPUTE_16F, CUDA_R_16F));
        CRD_CUDA_CHECK(cublasLtMatmulDescCreate(&linear_, CUBLAS_COMPUTE_16F, CUDA_R_16F));
        const cublasLtEpilogue_t epilogue = CUBLASLT_EPILOGUE_RELU;
        CRD_CUDA_CHECK(
            cublasLtMatmulDescSetAttribute(relu_, CUBLASLT_MATMUL_DESC_EPILOGUE, &epilogue, sizeof(epilogue)));
        CRD_CUDA_CHECK(cublasLtMatrixLayoutCreate(&weights_, CUDA_R_16F, kWidth, kWidth, kWidth));
        CRD_CUDA_CHECK(cublasLtMatrixLayoutCreate(&activations_, CUDA_R_16F, kWidth, kBatch, kWidth));
    }

    ~LtForward()
    {
        CRD_CUDA_CHECK(cublasLtMatrixLayoutDestroy(activations_));
        CRD_CUDA_CHECK(cublasLtMatrixLayoutDestroy(weights_));
        CRD_CUDA_CHECK(cublasLtMatmulDescDestroy(linear_));
        CRD_CUDA_CHECK(cublasLtMatmulDescDestroy(relu_));
        CRD_CUDA_CHECK(cublasLtDestroy(handle_));
    }

    LtForward(const LtForward&)            = delete;
    LtForward& operator=(const LtForward&) = delete;

    // out = act(W x in) for one layer.
    void layer(const __half* w, const __half* in, __half* out, bool relu) const
    {
        const cublasLtMatmulDesc_t op = relu ? relu_ : linear_;
        CRD_CUDA_CHECK(cublasLtMatmul(handle_, op, &one_, w, weights_, in, activations_, &zero_, out, activations_, out,
                                      activations_, nullptr, nullptr, 0, nullptr));
    }

    // The whole network from `in` to `out`, the hidden activations alternating between `ping` and `pong`.
    void network(const __half* w, const __half* in, __half* ping, __half* pong, __half* out) const
    {
        const __half* source = in;
        for (int l = 0; l < kLayers; ++l)
        {
            const bool last        = l + 1 == kLayers;
            __half*    destination = last ? out : (l % 2 == 0 ? ping : pong);
            layer(w + kMatrix * l, source, destination, !last);
            source = destination;
        }
    }

    // The whole network keeping every activation: acts[0] is the input, acts[l + 1] layer l's output.
    void network_keeping_activations(const __half* w, __half* acts) const
    {
        for (int l = 0; l < kLayers; ++l)
        {
            layer(w + kMatrix * l, acts + kActivations * l, acts + kActivations * (l + 1), l + 1 < kLayers);
        }
    }

    cublasLtHandle_t handle() const
    {
        return handle_;
    }

    cublasLtMatrixLayout_t weight_layout() const
    {
        return weights_;
    }

    cublasLtMatrixLayout_t activation_layout() const
    {
        return activations_;
    }

private:
    cublasLtHandle_t       handle_      = nullptr;
    cublasLtMatmulDesc_t   relu_        = nullptr;
    cublasLtMatmulDesc_t   linear_      = nullptr;
    cublasLtMatrixLayout_t weights_     = nullptr;
    cublasLtMatrixLayout_t activations_ = nullptr;
    __half                 one_         = __float2half(1.0F);
    __half                 zero_        = __float2half(0.0F);
};

// The vendor backward at its best: per layer the ReLU mask, the dW GEMM (fp32, K = the batch) and the da GEMM, each
// round-tripping DRAM, with a 256 MiB workspace and the algorithm cuBLASLt's heuristic ranks first for each shape
// (split-K for the dW reduction needs both).
class LtBackward
{
public:
    explicit LtBackward(const LtForward& forward)
        : forward_(forward), workspace_(device_array<unsigned char>(kWorkspaceBytes))
    {
        CRD_CUDA_CHECK(cublasLtMatmulPreferenceCreate(&preference_));
        CRD_CUDA_CHECK(cublasLtMatmulPreferenceSetAttribute(preference_, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES,
                                                            &kWorkspaceBytes, sizeof(kWorkspaceBytes)));
        // dW[l] (64 x 64, fp32) = a[l] . dz^T: A = a[l] and B = dz, both 64 x N column-major, B transposed.
        CRD_CUDA_CHECK(cublasLtMatmulDescCreate(&weight_op_, CUBLAS_COMPUTE_32F, CUDA_R_32F));
        const cublasOperation_t transpose = CUBLAS_OP_T;
        CRD_CUDA_CHECK(cublasLtMatmulDescSetAttribute(weight_op_, CUBLASLT_MATMUL_DESC_TRANSB, &transpose,
                                                      sizeof(transpose)));
        CRD_CUDA_CHECK(cublasLtMatrixLayoutCreate(&gradient_, CUDA_R_32F, kWidth, kWidth, kWidth));
        // da[l] (64 x N, fp16) = W[l] . dz, the forward's shape.
        CRD_CUDA_CHECK(cublasLtMatmulDescCreate(&activation_op_, CUBLAS_COMPUTE_16F, CUDA_R_16F));
        const cublasLtMatrixLayout_t acts = forward.activation_layout();
        weight_algo_                      = first_algorithm("dW", weight_op_, acts, acts, gradient_);
        activation_algo_ = first_algorithm("da", activation_op_, forward.weight_layout(), acts, acts);
    }

    ~LtBackward()
    {
        CRD_CUDA_CHECK(cublasLtMatmulDescDestroy(activation_op_));
        CRD_CUDA_CHECK(cublasLtMatrixLayoutDestroy(gradient_));
        CRD_CUDA_CHECK(cublasLtMatmulDescDestroy(weight_op_));
        CRD_CUDA_CHECK(cublasLtMatmulPreferenceDestroy(preference_));
    }

    LtBackward(const LtBackward&)            = delete;
    LtBackward& operator=(const LtBackward&) = delete;

    void mask(const __half* g, const __half* a_out, __half* dz, bool relu) const
    {
        constexpr int kBlock = 256;
        constexpr int kCount = static_cast<int>(kActivations);
        relu_mask<<<(kCount + kBlock - 1) / kBlock, kBlock>>>(g, a_out, dz, kCount, relu ? 1 : 0);
        CRD_CUDA_CHECK(cudaGetLastError());
    }

    void weight_gradient(const __half* a_in, const __half* dz, float* dw) const
    {
        const cublasLtMatrixLayout_t acts = forward_.activation_layout();
        CRD_CUDA_CHECK(cublasLtMatmul(forward_.handle(), weight_op_, &one_, a_in, acts, dz, acts, &zero_, dw,
                                      gradient_, dw, gradient_, &weight_algo_, workspace_.get(), kWorkspaceBytes,
                                      nullptr));
    }

    void activation_gradient(const __half* w, const __half* dz, __half* da) const
    {
        const cublasLtMatrixLayout_t acts = forward_.activation_layout();
        CRD_CUDA_CHECK(cublasLtMatmul(forward_.handle(), activation_op_, &half_one_, w, forward_.weight_layout(), dz,
                                      acts, &half_zero_, da, acts, da, acts, &activation_algo_, workspace_.get(),
                                      kWorkspaceBytes, nullptr));
    }

    // The whole backward pass from dL/dout = `gout` over the kept activations; dw receives every layer's gradient.
    void network(const __half* w, const __half* acts, const __half* gout, __half* dz, __half* da, float* dw) const
    {
        CRD_CUDA_CHECK(cudaMemset(dw, 0, sizeof(float) * kWeights));
        const __half* g = gout;
        for (int l = kLayers - 1; l >= 0; --l)
        {
            mask(g, acts + kActivations * (l + 1), dz, l + 1 < kLayers);
            weight_gradient(acts + kActivations * l, dz, dw + kMatrix * l);
            activation_gradient(w + kMatrix * l, dz, da);
            g = da;
        }
    }

private:
    static constexpr size_t kWorkspaceBytes = static_cast<size_t>(256) << 20;

    cublasLtMatmulAlgo_t first_algorithm(const char* what, cublasLtMatmulDesc_t op, cublasLtMatrixLayout_t a,
                                         cublasLtMatrixLayout_t b, cublasLtMatrixLayout_t c) const
    {
        cublasLtMatmulHeuristicResult_t result{};
        int                             found = 0;
        CRD_CUDA_CHECK(
            cublasLtMatmulAlgoGetHeuristic(forward_.handle(), op, a, b, c, c, preference_, 1, &result, &found));
        if (found == 0)
        {
            std::fprintf(stderr, "cuBLASLt offers no algorithm for the %s GEMM\n", what);
            std::exit(1);
        }
        return result.algo;
    }

    const LtForward&           forward_;
    DeviceArray<unsigned char> workspace_;
    cublasLtMatmulPreference_t preference_      = nullptr;
    cublasLtMatmulDesc_t       weight_op_       = nullptr;
    cublasLtMatmulDesc_t       activation_op_   = nullptr;
    cublasLtMatrixLayout_t     gradient_        = nullptr;
    cublasLtMatmulAlgo_t       weight_algo_     = {};
    cublasLtMatmulAlgo_t       activation_algo_ = {};
    float                      one_             = 1.0F;
    float                      zero_            = 0.0F;
    __half                     half_one_        = __float2half(1.0F);
    __half                     half_zero_       = __float2half(0.0F);
};

// The fused output against cuBLASLt's: the same fp16 math in a different accumulation order agrees within 8% of the
// largest reference magnitude, which must itself be nonzero (an all-zero pair agrees with nothing).
inline bool outputs_agree(const char* what, const __half* reference, const __half* candidate)
{
    double max_difference = 0.0;
    double max_magnitude  = 0.0;
    for (size_t i = 0; i < kActivations; ++i)
    {
        const double r          = __half2float(reference[i]);
        const double c          = __half2float(candidate[i]);
        const double difference = r > c ? r - c : c - r;
        const double magnitude  = r < 0.0 ? -r : r;
        max_difference          = difference > max_difference ? difference : max_difference;
        max_magnitude           = magnitude > max_magnitude ? magnitude : max_magnitude;
    }
    const double relative = max_difference / (max_magnitude > 0.0 ? max_magnitude : 1.0);
    std::printf("# correctness (%s): max_abs_diff=%.5f max_ref_mag=%.5f rel=%.5f\n", what, max_difference,
                max_magnitude, relative);
    if (max_magnitude < 1e-3 || relative > 0.08)
    {
        std::fprintf(stderr, "%s: the outputs disagree (rel %.5f, largest reference magnitude %.5f)\n", what,
                     relative, max_magnitude);
        return false;
    }
    return true;
}

// The CPU oracle of the output layer's weight gradient, dW[L-1] = a[L-1]^T . gout (the linear output layer has no
// mask, so dz[L-1] = gout), as 64 x 64 fp32 stored [k + 64 n].
inline std::unique_ptr<float[]> last_layer_weight_gradient(const __half* acts, const __half* gout)
{
    const auto a        = download(acts + kActivations * (kLayers - 1), kActivations);
    const auto g        = download(gout, kActivations);
    auto       gradient = std::make_unique<float[]>(kMatrix);
    for (size_t i = 0; i < kMatrix; ++i)
    {
        gradient[i] = 0.0F;
    }
    for (size_t r = 0; r < static_cast<size_t>(kBatch); ++r)
    {
        for (int k = 0; k < kWidth; ++k)
        {
            const float value = __half2float(a[r * kWidth + k]);
            for (int n = 0; n < kWidth; ++n)
            {
                gradient[k + kWidth * n] += value * __half2float(g[r * kWidth + n]);
            }
        }
    }
    return gradient;
}

// A device dW against the oracle: within 2% of the largest reference magnitude, which must exceed 1e-3.
inline bool weight_gradient_agrees(const char* what, const float* dw, const float* reference)
{
    const auto got            = download(dw + kMatrix * (kLayers - 1), kMatrix);
    double     max_difference = 0.0;
    double     max_magnitude  = 0.0;
    for (size_t i = 0; i < kMatrix; ++i)
    {
        const double r          = reference[i];
        const double c          = got[i];
        const double difference = r > c ? r - c : c - r;
        const double magnitude  = r < 0.0 ? -r : r;
        max_difference          = difference > max_difference ? difference : max_difference;
        max_magnitude           = magnitude > max_magnitude ? magnitude : max_magnitude;
    }
    const double relative = max_difference / (max_magnitude > 0.0 ? max_magnitude : 1.0);
    std::printf("# %s dW[L-1]: max_abs_diff=%.3f max_ref_mag=%.3f rel=%.5f\n", what, max_difference, max_magnitude,
                relative);
    if (max_magnitude <= 1e-3 || relative > 0.02)
    {
        std::fprintf(stderr, "%s: dW[L-1] disagrees with the CPU oracle (rel %.5f, largest magnitude %.3f)\n", what,
                     relative, max_magnitude);
        return false;
    }
    return true;
}

// Three warm-up calls, then the least host time per call over 7 runs of 50 back-to-back calls (best_wall_ms), printed
// as a row with the fp16 rate `flops` gives.
template <typename Body>
double time_row(const char* name, double flops, Body&& body)
{
    for (int call = 0; call < 3; ++call)
    {
        body();
    }
    const double best = best_wall_ms(7, 50, body);
    std::printf("%-20s %.5f ms   %.1f TFLOP/s (fp16)\n", name, best, flops / (best * 1.0e9));
    return best;
}

// The vendor backward's three parts timed on their own, to show which shape dominates its time (each row rated at
// the whole pass's flops, as the board prints them).
inline void time_backward_parts(const LtBackward& backward, const __half* w, const __half* acts, const __half* gout,
                                __half* dz, __half* da, float* dw)
{
    const double flops  = 4.0 * kBatch * kWidth * kWidth * kLayers;
    const double t_mask = time_row("  cuBLAS mask-only", flops, [&]
    {
        const __half* g = gout;
        for (int l = kLayers - 1; l >= 0; --l)
        {
            backward.mask(g, acts + kActivations * (l + 1), dz, l + 1 < kLayers);
            g = da;
        }
    });
    const double t_dw = time_row("  cuBLAS dW-only", flops, [&]
    {
        for (int l = kLayers - 1; l >= 0; --l)
        {
            backward.weight_gradient(acts + kActivations * l, dz, dw + kMatrix * l);
        }
    });
    const double t_da = time_row("  cuBLAS dA-only", flops, [&]
    {
        for (int l = kLayers - 1; l >= 0; --l)
        {
            backward.activation_gradient(w + kMatrix * l, dz, da);
        }
    });
    std::printf("# breakdown: mask %.2f + dW %.2f + dA %.2f ms\n", t_mask, t_dw, t_da);
}
} // namespace crd_bench::mlp
