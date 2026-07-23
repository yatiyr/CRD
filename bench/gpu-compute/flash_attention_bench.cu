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
// against a double-precision CPU reference (the accepted comparison; flash IS the reference method for large-S attention).
// Build: nvcc -O3 -std=c++17 -arch=sm_89 -allow-unsupported-compiler flash_attention_bench.cu -o flash_attention_bench.exe

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CUDA_OK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { std::printf("CUDA err %s @ %d: %s\n", #x, __LINE__, cudaGetErrorString(e_)); std::exit(1); } } while (0)

static constexpr int D = 64; // head dim (fixed; a warp-friendly, register-fittable size)

// ── UNFUSED baseline: 3 kernels that materialise the S×S scores to DRAM ────────────────────────────────────────────────────────
__global__ void qk_scores(const float* Q, const float* K, float* Sc, int S, float scale)
{
    const int i = blockIdx.y * blockDim.y + threadIdx.y; // query row
    const int j = blockIdx.x * blockDim.x + threadIdx.x; // key row
    if (i >= S || j >= S) { return; }
    float dot = 0.0f;
    for (int d = 0; d < D; ++d) { dot += Q[i * D + d] * K[j * D + d]; }
    Sc[i * S + j] = dot * scale;
}
__global__ void softmax_rows(float* Sc, int S)
{
    const int i = blockIdx.x; // one block per row
    const int t = threadIdx.x;
    extern __shared__ float red[];
    float m = -INFINITY;
    for (int j = t; j < S; j += blockDim.x) { m = fmaxf(m, Sc[i * S + j]); }
    red[t] = m; __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) { if (t < s) { red[t] = fmaxf(red[t], red[t + s]); } __syncthreads(); }
    const float rowmax = red[0]; __syncthreads();
    float l = 0.0f;
    for (int j = t; j < S; j += blockDim.x) { const float e = expf(Sc[i * S + j] - rowmax); Sc[i * S + j] = e; l += e; }
    red[t] = l; __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) { if (t < s) { red[t] += red[t + s]; } __syncthreads(); }
    const float rowsum = red[0]; __syncthreads();
    for (int j = t; j < S; j += blockDim.x) { Sc[i * S + j] /= rowsum; }
}
__global__ void pv_matmul(const float* P, const float* V, float* O, int S)
{
    const int i = blockIdx.y * blockDim.y + threadIdx.y; // query row
    const int d = blockIdx.x * blockDim.x + threadIdx.x; // out dim
    if (i >= S || d >= D) { return; }
    float acc = 0.0f;
    for (int j = 0; j < S; ++j) { acc += P[i * S + j] * V[j * D + d]; }
    O[i * D + d] = acc;
}

// ── FLASH (fused, online softmax, tiled) — one block per Br query rows; Bc-wide key/value tiles staged in shared ────────────────
template <int BR, int BC>
__global__ void flash_attention(const float* Q, const float* K, const float* V, float* O, int S, float scale)
{
    __shared__ float Ks[BC][D];
    __shared__ float Vs[BC][D];
    const int qi = blockIdx.x * BR + threadIdx.x; // this thread owns one query row
    const int t  = threadIdx.x;

    float q[D];
    if (qi < S) { for (int d = 0; d < D; ++d) { q[d] = Q[qi * D + d]; } }
    float acc[D];
    for (int d = 0; d < D; ++d) { acc[d] = 0.0f; }
    float m = -INFINITY;
    float l = 0.0f;

    for (int kt = 0; kt < S; kt += BC)
    {
        // cooperative load of the K/V tile (BR threads load BC·D elements) into shared
        for (int idx = t; idx < BC * D; idx += BR)
        {
            const int r = idx / D;
            const int c = idx % D;
            const int kg = kt + r;
            Ks[r][c] = kg < S ? K[kg * D + c] : 0.0f;
            Vs[r][c] = kg < S ? V[kg * D + c] : 0.0f;
        }
        __syncthreads();

        if (qi < S)
        {
            float s[BC];
            float tile_max = -INFINITY;
            const int ncols = (kt + BC <= S) ? BC : (S - kt);
            for (int j = 0; j < ncols; ++j)
            {
                float dot = 0.0f;
                for (int d = 0; d < D; ++d) { dot += q[d] * Ks[j][d]; }
                s[j]     = dot * scale;
                tile_max = fmaxf(tile_max, s[j]);
            }
            const float m_new = fmaxf(m, tile_max);
            const float corr  = expf(m - m_new); // rescale prior l + O to the new max
            float       l_tile = 0.0f;
            for (int j = 0; j < ncols; ++j) { s[j] = expf(s[j] - m_new); l_tile += s[j]; }
            l = l * corr + l_tile;
            for (int d = 0; d < D; ++d) { acc[d] *= corr; }
            for (int j = 0; j < ncols; ++j) { for (int d = 0; d < D; ++d) { acc[d] += s[j] * Vs[j][d]; } }
            m = m_new;
        }
        __syncthreads(); // before the next tile overwrites Ks/Vs
    }
    if (qi < S) { for (int d = 0; d < D; ++d) { O[qi * D + d] = acc[d] / l; } }
}

// ── double-precision CPU reference (the gold) ──────────────────────────────────────────────────────────────────────────────────
static void cpu_reference(const float* Q, const float* K, const float* V, float* O, int S, float scale)
{
    double* row = new double[S];
    for (int i = 0; i < S; ++i)
    {
        double mx = -1e300;
        for (int j = 0; j < S; ++j)
        {
            double dot = 0.0;
            for (int d = 0; d < D; ++d) { dot += static_cast<double>(Q[i * D + d]) * K[j * D + d]; }
            row[j] = dot * scale;
            if (row[j] > mx) { mx = row[j]; }
        }
        double sum = 0.0;
        for (int j = 0; j < S; ++j) { row[j] = std::exp(row[j] - mx); sum += row[j]; }
        for (int d = 0; d < D; ++d)
        {
            double acc = 0.0;
            for (int j = 0; j < S; ++j) { acc += row[j] * V[j * D + d]; }
            O[i * D + d] = static_cast<float>(acc / sum);
        }
    }
    delete[] row;
}

static float max_abs_err(const float* a, const float* b, int n)
{
    float e = 0.0f;
    for (int i = 0; i < n; ++i) { e = fmaxf(e, fabsf(a[i] - b[i])); }
    return e;
}

int main()
{
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# FLASH vs UNFUSED attention (D=%d, f32) — %s\n", D, prop.name);
    std::printf("# %-6s %-13s %-13s %-9s %-11s %-11s\n", "S", "flash_ms", "unfused_ms", "CRUSH", "flash_err", "unfused_err");

    const int sizes[] = {512, 1024, 2048, 4096};
    for (int si = 0; si < 4; ++si)
    {
        const int   S     = sizes[si];
        const float scale = 1.0f / std::sqrt(static_cast<float>(D));
        const size_t nqkv = static_cast<size_t>(S) * D;
        const size_t nsc  = static_cast<size_t>(S) * S;

        float* hQ = new float[nqkv]; float* hK = new float[nqkv]; float* hV = new float[nqkv];
        for (size_t i = 0; i < nqkv; ++i)
        {
            hQ[i] = static_cast<float>((static_cast<int>((i * 1103515245u + 12345u) >> 16) & 255) - 128) / 256.0f;
            hK[i] = static_cast<float>((static_cast<int>((i * 1664525u + 1013904223u) >> 16) & 255) - 128) / 256.0f;
            hV[i] = static_cast<float>((static_cast<int>((i * 22695477u + 1u) >> 16) & 255) - 128) / 256.0f;
        }
        float* hO_ref   = new float[nqkv];
        float* hO_flash = new float[nqkv];
        float* hO_unf   = new float[nqkv];
        cpu_reference(hQ, hK, hV, hO_ref, S, scale);

        float *dQ, *dK, *dV, *dO, *dSc;
        CUDA_OK(cudaMalloc(&dQ, nqkv * 4)); CUDA_OK(cudaMalloc(&dK, nqkv * 4)); CUDA_OK(cudaMalloc(&dV, nqkv * 4));
        CUDA_OK(cudaMalloc(&dO, nqkv * 4)); CUDA_OK(cudaMalloc(&dSc, nsc * 4));
        CUDA_OK(cudaMemcpy(dQ, hQ, nqkv * 4, cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(dK, hK, nqkv * 4, cudaMemcpyHostToDevice));
        CUDA_OK(cudaMemcpy(dV, hV, nqkv * 4, cudaMemcpyHostToDevice));

        constexpr int BR = 64, BC = 32;
        auto run_flash = [&]() { flash_attention<BR, BC><<<S / BR, BR>>>(dQ, dK, dV, dO, S, scale); };
        auto run_unfused = [&]() {
            dim3 tb(16, 16); dim3 gs((S + 15) / 16, (S + 15) / 16);
            qk_scores<<<gs, tb>>>(dQ, dK, dSc, S, scale);
            softmax_rows<<<S, 256, 256 * 4>>>(dSc, S);
            dim3 gp((D + 15) / 16, (S + 15) / 16);
            pv_matmul<<<gp, tb>>>(dSc, dV, dO, S);
        };

        // correctness
        run_flash(); CUDA_OK(cudaDeviceSynchronize()); CUDA_OK(cudaMemcpy(hO_flash, dO, nqkv * 4, cudaMemcpyDeviceToHost));
        run_unfused(); CUDA_OK(cudaDeviceSynchronize()); CUDA_OK(cudaMemcpy(hO_unf, dO, nqkv * 4, cudaMemcpyDeviceToHost));
        const float ferr = max_abs_err(hO_flash, hO_ref, static_cast<int>(nqkv));
        const float uerr = max_abs_err(hO_unf, hO_ref, static_cast<int>(nqkv));

        // timing (min of 30, 5 warmup)
        cudaEvent_t a, b; CUDA_OK(cudaEventCreate(&a)); CUDA_OK(cudaEventCreate(&b));
        auto timeit = [&](auto fn) {
            for (int w = 0; w < 5; ++w) { fn(); }
            CUDA_OK(cudaDeviceSynchronize());
            float best = 1e30f;
            for (int r = 0; r < 30; ++r)
            {
                CUDA_OK(cudaEventRecord(a)); fn(); CUDA_OK(cudaEventRecord(b)); CUDA_OK(cudaEventSynchronize(b));
                float ms = 0.0f; CUDA_OK(cudaEventElapsedTime(&ms, a, b)); if (ms < best) { best = ms; }
            }
            return best;
        };
        const float fms = timeit(run_flash);
        const float ums = timeit(run_unfused);

        std::printf("%-6d %-13.5f %-13.5f %-9.3f %-11.2e %-11.2e\n", S, fms, ums, ums / fms, ferr, uerr);

        cudaEventDestroy(a); cudaEventDestroy(b);
        cudaFree(dQ); cudaFree(dK); cudaFree(dV); cudaFree(dO); cudaFree(dSc);
        delete[] hQ; delete[] hK; delete[] hV; delete[] hO_ref; delete[] hO_flash; delete[] hO_unf;
    }
    return 0;
}
