// flash_attention_bench.cu — AS-4 attention FUSION crush prototype (de-risking the algorithm before the CKIR emitter).
// Scaled dot-product attention  O = softmax(Q·Kᵀ·scale)·V,  Q,K,V,O ∈ [S,D] (single head, scale = 1/sqrt(D)).
//
//   UNFUSED (the peer everyone pays): 3 kernels that MATERIALIZE the S×S scores to DRAM — (1) scores = Q·Kᵀ·scale, (2) softmax
//   in place, (3) O = P·V. Global traffic ≈ 3·S² + 4·S·D. For large S the S×S round-trip DOMINATES (memory-bound).
//
//   FLASH (fused, FlashAttention-2 style): ONE kernel, one block per Br-row query tile, streaming Bc-column key/value tiles through
//   SHARED memory with an ONLINE softmax (running max m, running sum l, rescaled output accumulator). The S×S scores NEVER touch
//   DRAM — traffic ≈ 4·S·D. That is the structural moat: the fusion removes the O(S²) memory the unfused peer cannot avoid.
//
// Online softmax REASSOCIATES the row reduction, so flash is a FAST tier (not bit-exact vs the naive) — validated to tolerance
// against a double-precision CPU reference (the accepted comparison; flash IS the reference method for large-S
// attention): an output further than kTolerance from the reference fails the run. Built by CRD_BUILD_GPU_VS_CUDA
// (bench/CMakeLists.txt) and run by scripts/run_bench_gpu_cuda.sh.

#include "cuda_bench.hpp"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
constexpr int   kHeadDim   = 64;   // D: head dim (fixed; a warp-friendly, register-fittable size)
constexpr float kTolerance = 1e-5f; // the largest |O - O_ref| accepted (outputs are averages of values in [-0.5, 0.5])

// ── UNFUSED baseline: 3 kernels that materialise the S×S scores to DRAM ────────────────────────────────────────────────────────
__global__ void qk_scores(const float* q, const float* k, float* scores, int s, float scale)
{
    const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y); // query row
    const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x); // key row
    if (i >= s || j >= s)
    {
        return;
    }
    float dot = 0.0f;
    for (int d = 0; d < kHeadDim; ++d)
    {
        dot += q[i * kHeadDim + d] * k[j * kHeadDim + d];
    }
    scores[i * s + j] = dot * scale;
}

__global__ void softmax_rows(float* scores, int s)
{
    const int i       = static_cast<int>(blockIdx.x); // one block per row
    const int t       = static_cast<int>(threadIdx.x);
    const int threads = static_cast<int>(blockDim.x);
    extern __shared__ float red[];
    float                   m = -INFINITY;
    for (int j = t; j < s; j += threads)
    {
        m = fmaxf(m, scores[i * s + j]);
    }
    red[t] = m;
    __syncthreads();
    for (int half = threads / 2; half > 0; half >>= 1)
    {
        if (t < half)
        {
            red[t] = fmaxf(red[t], red[t + half]);
        }
        __syncthreads();
    }
    const float rowmax = red[0];
    __syncthreads();
    float l = 0.0f;
    for (int j = t; j < s; j += threads)
    {
        const float e     = expf(scores[i * s + j] - rowmax);
        scores[i * s + j] = e;
        l += e;
    }
    red[t] = l;
    __syncthreads();
    for (int half = threads / 2; half > 0; half >>= 1)
    {
        if (t < half)
        {
            red[t] += red[t + half];
        }
        __syncthreads();
    }
    const float rowsum = red[0];
    __syncthreads();
    for (int j = t; j < s; j += threads)
    {
        scores[i * s + j] /= rowsum;
    }
}

__global__ void pv_matmul(const float* p, const float* v, float* o, int s)
{
    const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y); // query row
    const int d = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x); // out dim
    if (i >= s || d >= kHeadDim)
    {
        return;
    }
    float acc = 0.0f;
    for (int j = 0; j < s; ++j)
    {
        acc += p[i * s + j] * v[j * kHeadDim + d];
    }
    o[i * kHeadDim + d] = acc;
}

// ── FLASH (fused, online softmax, tiled) — one block per Br query rows; Bc-wide key/value tiles staged in shared ────────────────
template <int BR, int BC>
__global__ void flash_attention(const float* q_in, const float* k_in, const float* v_in, float* o, int s, float scale)
{
    __shared__ float ks[BC][kHeadDim];
    __shared__ float vs[BC][kHeadDim];
    const int        qi = static_cast<int>(blockIdx.x) * BR + static_cast<int>(threadIdx.x); // one query row per thread
    const int        t  = static_cast<int>(threadIdx.x);

    float q[kHeadDim];
    if (qi < s)
    {
        for (int d = 0; d < kHeadDim; ++d)
        {
            q[d] = q_in[qi * kHeadDim + d];
        }
    }
    float acc[kHeadDim];
    for (int d = 0; d < kHeadDim; ++d)
    {
        acc[d] = 0.0f;
    }
    float m = -INFINITY;
    float l = 0.0f;

    for (int kt = 0; kt < s; kt += BC)
    {
        // cooperative load of the K/V tile (BR threads load BC·D elements) into shared
        for (int idx = t; idx < BC * kHeadDim; idx += BR)
        {
            const int r  = idx / kHeadDim;
            const int c  = idx % kHeadDim;
            const int kg = kt + r;
            ks[r][c]     = kg < s ? k_in[kg * kHeadDim + c] : 0.0f;
            vs[r][c]     = kg < s ? v_in[kg * kHeadDim + c] : 0.0f;
        }
        __syncthreads();

        if (qi < s)
        {
            float     scores[BC];
            float     tile_max = -INFINITY;
            const int ncols    = (kt + BC <= s) ? BC : (s - kt);
            for (int j = 0; j < ncols; ++j)
            {
                float dot = 0.0f;
                for (int d = 0; d < kHeadDim; ++d)
                {
                    dot += q[d] * ks[j][d];
                }
                scores[j] = dot * scale;
                tile_max  = fmaxf(tile_max, scores[j]);
            }
            const float m_new  = fmaxf(m, tile_max);
            const float corr   = expf(m - m_new); // rescale prior l + O to the new max
            float       l_tile = 0.0f;
            for (int j = 0; j < ncols; ++j)
            {
                scores[j] = expf(scores[j] - m_new);
                l_tile += scores[j];
            }
            l = l * corr + l_tile;
            for (int d = 0; d < kHeadDim; ++d)
            {
                acc[d] *= corr;
            }
            for (int j = 0; j < ncols; ++j)
            {
                for (int d = 0; d < kHeadDim; ++d)
                {
                    acc[d] += scores[j] * vs[j][d];
                }
            }
            m = m_new;
        }
        __syncthreads(); // before the next tile overwrites ks/vs
    }
    if (qi < s)
    {
        for (int d = 0; d < kHeadDim; ++d)
        {
            o[qi * kHeadDim + d] = acc[d] / l;
        }
    }
}

// ── double-precision CPU reference (the gold) ──────────────────────────────────────────────────────────────────────────────────
void cpu_reference(const float* q, const float* k, const float* v, float* o, int s, float scale)
{
    const auto row = std::make_unique<double[]>(static_cast<size_t>(s));
    for (int i = 0; i < s; ++i)
    {
        double mx = -1e300;
        for (int j = 0; j < s; ++j)
        {
            double dot = 0.0;
            for (int d = 0; d < kHeadDim; ++d)
            {
                dot += static_cast<double>(q[i * kHeadDim + d]) * static_cast<double>(k[j * kHeadDim + d]);
            }
            row[j] = dot * static_cast<double>(scale);
            if (row[j] > mx)
            {
                mx = row[j];
            }
        }
        double sum = 0.0;
        for (int j = 0; j < s; ++j)
        {
            row[j] = std::exp(row[j] - mx);
            sum += row[j];
        }
        for (int d = 0; d < kHeadDim; ++d)
        {
            double acc = 0.0;
            for (int j = 0; j < s; ++j)
            {
                acc += row[j] * static_cast<double>(v[j * kHeadDim + d]);
            }
            o[i * kHeadDim + d] = static_cast<float>(acc / sum);
        }
    }
}

float max_abs_err(const float* a, const float* b, size_t n)
{
    float e = 0.0f;
    for (size_t i = 0; i < n; ++i)
    {
        e = fmaxf(e, fabsf(a[i] - b[i]));
    }
    return e;
}

float lcg_value(size_t i, unsigned multiplier, unsigned increment)
{
    const unsigned state = static_cast<unsigned>(i) * multiplier + increment;
    return static_cast<float>(static_cast<int>((state >> 16) & 255U) - 128) / 256.0f;
}
} // namespace

int main()
{
    cudaDeviceProp prop{};
    CRD_CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("# FLASH vs UNFUSED attention (D=%d, f32) — %s\n", kHeadDim, prop.name);
    std::printf("# %-6s %-13s %-13s %-9s %-11s %-11s\n", "S", "flash_ms", "unfused_ms", "CRUSH", "flash_err", "unfused_err");

    const int sizes[] = {512, 1024, 2048, 4096};
    for (const int s : sizes)
    {
        const float  scale       = 1.0f / std::sqrt(static_cast<float>(kHeadDim));
        const size_t nqkv        = static_cast<size_t>(s) * kHeadDim;
        const size_t nsc         = static_cast<size_t>(s) * static_cast<size_t>(s);
        const size_t qkv_bytes   = nqkv * sizeof(float);
        const auto   h_q         = std::make_unique<float[]>(nqkv);
        const auto   h_k         = std::make_unique<float[]>(nqkv);
        const auto   h_v         = std::make_unique<float[]>(nqkv);
        const auto   h_o_ref     = std::make_unique<float[]>(nqkv);
        const auto   h_o_flash   = std::make_unique<float[]>(nqkv);
        const auto   h_o_unfused = std::make_unique<float[]>(nqkv);
        for (size_t i = 0; i < nqkv; ++i)
        {
            h_q[i] = lcg_value(i, 1103515245U, 12345U);
            h_k[i] = lcg_value(i, 1664525U, 1013904223U);
            h_v[i] = lcg_value(i, 22695477U, 1U);
        }
        cpu_reference(h_q.get(), h_k.get(), h_v.get(), h_o_ref.get(), s, scale);

        float* d_q      = nullptr;
        float* d_k      = nullptr;
        float* d_v      = nullptr;
        float* d_o      = nullptr;
        float* d_scores = nullptr;
        CRD_CUDA_CHECK(cudaMalloc(&d_q, qkv_bytes));
        CRD_CUDA_CHECK(cudaMalloc(&d_k, qkv_bytes));
        CRD_CUDA_CHECK(cudaMalloc(&d_v, qkv_bytes));
        CRD_CUDA_CHECK(cudaMalloc(&d_o, qkv_bytes));
        CRD_CUDA_CHECK(cudaMalloc(&d_scores, nsc * sizeof(float)));
        CRD_CUDA_CHECK(cudaMemcpy(d_q, h_q.get(), qkv_bytes, cudaMemcpyHostToDevice));
        CRD_CUDA_CHECK(cudaMemcpy(d_k, h_k.get(), qkv_bytes, cudaMemcpyHostToDevice));
        CRD_CUDA_CHECK(cudaMemcpy(d_v, h_v.get(), qkv_bytes, cudaMemcpyHostToDevice));

        constexpr int  kBr       = 64;
        constexpr int  kBc       = 32;
        const unsigned rows      = static_cast<unsigned>(s);
        const auto     run_flash = [&]
        {
            flash_attention<kBr, kBc><<<rows / kBr, kBr>>>(d_q, d_k, d_v, d_o, s, scale);
            CRD_CUDA_CHECK(cudaGetLastError());
        };
        const auto run_unfused = [&]
        {
            const dim3 tile(16, 16);
            const dim3 score_grid((rows + 15) / 16, (rows + 15) / 16);
            qk_scores<<<score_grid, tile>>>(d_q, d_k, d_scores, s, scale);
            CRD_CUDA_CHECK(cudaGetLastError());
            softmax_rows<<<rows, 256, 256 * sizeof(float)>>>(d_scores, s);
            CRD_CUDA_CHECK(cudaGetLastError());
            const dim3 output_grid((kHeadDim + 15) / 16, (rows + 15) / 16);
            pv_matmul<<<output_grid, tile>>>(d_scores, d_v, d_o, s);
            CRD_CUDA_CHECK(cudaGetLastError());
        };

        // correctness
        run_flash();
        CRD_CUDA_CHECK(cudaMemcpy(h_o_flash.get(), d_o, qkv_bytes, cudaMemcpyDeviceToHost));
        run_unfused();
        CRD_CUDA_CHECK(cudaMemcpy(h_o_unfused.get(), d_o, qkv_bytes, cudaMemcpyDeviceToHost));
        const float flash_err   = max_abs_err(h_o_flash.get(), h_o_ref.get(), nqkv);
        const float unfused_err = max_abs_err(h_o_unfused.get(), h_o_ref.get(), nqkv);
        if (!(flash_err <= kTolerance) || !(unfused_err <= kTolerance))
        {
            std::fprintf(stderr, "S=%d: flash error %.3e, unfused error %.3e against the reference (tolerance %.0e)\n",
                         s, static_cast<double>(flash_err), static_cast<double>(unfused_err),
                         static_cast<double>(kTolerance));
            return 1;
        }

        // timing (min of 30, 5 warmup)
        const auto timeit = [](const auto& run)
        {
            for (int w = 0; w < 5; ++w)
            {
                run();
            }
            CRD_CUDA_CHECK(cudaDeviceSynchronize());
            return crd_bench::best_event_ms(30, run);
        };
        const float flash_ms   = timeit(run_flash);
        const float unfused_ms = timeit(run_unfused);

        std::printf("%-6d %-13.5f %-13.5f %-9.3f %-11.2e %-11.2e\n", s, static_cast<double>(flash_ms),
                    static_cast<double>(unfused_ms), static_cast<double>(unfused_ms / flash_ms),
                    static_cast<double>(flash_err), static_cast<double>(unfused_err));

        for (float* buffer : {d_q, d_k, d_v, d_o, d_scores})
        {
            CRD_CUDA_CHECK(cudaFree(buffer));
        }
    }
    return 0;
}
