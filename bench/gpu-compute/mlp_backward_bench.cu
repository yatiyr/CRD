// mlp_backward_bench.cu — THE TRAINING HALF OF THE NRC MOAT: a FULLY-FUSED MLP BACKWARD pass (activation gradients never
// leave the chip) vs NVIDIA's vendor backward pipeline (cublasLt: per layer, an elementwise ReLU'-mask + a da GEMM +
// a dW GEMM, all round-tripping DRAM). Same structural thesis as the forward crush: cuBLAS writes every dz/da intermediate
// to DRAM between calls; the fused kernel walks all layers backward with the gradient tile resident in shared.
//
// Net: input 64 -> 5 hidden 64 (ReLU) -> 64 linear out.  Loss = 1/2||out-target||^2, target = 0 => dL/dout = out.
// w[l] stored w[l][k + 64*n] = W[l][k][n].  Backward per layer l (a[l+1] = act(z[l]), z[l] = a[l] . W[l]):
//   dz[l]      = g (.) act'(z[l])   (act' = 1 for the linear output layer, (a[l+1]>0) for ReLU)
//   dW[l][k,n] = sum_r a[l][r,k] * dz[l][r,n]                 (a[l]^T . dz[l], 64x64, REDUCED over batch -> fp32 atomic)
//   da[l][r,k] = sum_n dz[l][r,n] * W[l][k,n]                 (dz . W^T -> new g, stays on-chip)
// Build: nvcc -O3 -std=c++17 -arch=sm_89 -allow-unsupported-compiler -Xcompiler /Zc:preprocessor
//        mlp_backward_bench.cu -o mlp_backward_bench.exe -lcublasLt -lcublas
#include <cublasLt.h>
#include <cuda_fp16.h>
#include <mma.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace nvcuda;

static void check(cudaError_t e, const char* w)
{
    if (e != cudaSuccess) { std::printf("CUDA ERR %s: %s\n", w, cudaGetErrorString(e)); std::exit(1); }
}
static void checkLt(cublasStatus_t e, const char* w)
{
    if (e != CUBLAS_STATUS_SUCCESS) { std::printf("cublasLt ERR %s: %d\n", w, static_cast<int>(e)); std::exit(1); }
}

constexpr int W_DIM  = 64;
constexpr int LAYERS = 6;
constexpr int BATCH  = 1 << 20;
constexpr int TILE   = 128;
constexpr int WARPS  = 2;
#ifndef NGROUP
#define NGROUP 8 // dW partial-accumulation groups (measured best; stays L2-resident, mild contention relief)
#endif
constexpr int DWSZ   = W_DIM * W_DIM;        // 64x64
constexpr int LDW    = LAYERS * DWSZ;        // per-group dW block

// reduce the NGROUP partial dW copies into the final dW
extern "C" __global__ void reduce_dw(const float* __restrict__ partial, float* __restrict__ dw)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < LDW)
    {
        float acc = 0.0F;
        for (int gp = 0; gp < NGROUP; ++gp) { acc += partial[static_cast<size_t>(gp) * LDW + i]; }
        dw[i] = acc;
    }
}

// dz = g (.) (a_out > 0), or dz = g if !do_mask (linear layer)
extern "C" __global__ void relu_mask(const __half* __restrict__ g, const __half* __restrict__ a_out,
                                     __half* __restrict__ dz, int n, int do_mask)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) { dz[i] = (do_mask && !__hgt(a_out[i], __float2half(0.0F))) ? __float2half(0.0F) : g[i]; }
}

// ── THE FUSED BACKWARD MLP ────────────────────────────────────────────────────────────────────────────────────────────
extern "C" __global__ void __launch_bounds__(WARPS * 32)
    fused_mlp_bwd(const __half* __restrict__ acts, const __half* __restrict__ w, const __half* __restrict__ gout,
                  float* __restrict__ dw)
{
    __shared__ __half sg_a[TILE * W_DIM];
    __shared__ __half sg_b[TILE * W_DIM];
    __shared__ __half sw[W_DIM * W_DIM];
    __shared__ float  sdw[WARPS][16 * 16]; // per-warp: each warp stores its own dW fragment before the atomic scatter

    const int block_row = blockIdx.x * TILE;
    const int tid       = threadIdx.x;
    const int warp      = tid / 32;

    {
        const __half* src = gout + static_cast<size_t>(block_row) * W_DIM;
        for (int i = tid; i < TILE * W_DIM / 8; i += WARPS * 32)
        {
            reinterpret_cast<float4*>(sg_a)[i] = reinterpret_cast<const float4*>(src)[i];
        }
    }
    __half* cur = sg_a;
    __half* nxt = sg_b;
    __syncthreads();

    for (int layer = LAYERS - 1; layer >= 0; --layer)
    {
        const __half* wl   = w + static_cast<size_t>(layer) * W_DIM * W_DIM;
        const __half* aIn  = acts + (static_cast<size_t>(layer) * BATCH + block_row) * W_DIM;
        const __half* aOut = acts + (static_cast<size_t>(layer + 1) * BATCH + block_row) * W_DIM;
        for (int i = tid; i < W_DIM * W_DIM / 8; i += WARPS * 32)
        {
            reinterpret_cast<float4*>(sw)[i] = reinterpret_cast<const float4*>(wl)[i];
        }
        __syncthreads();

        if (layer + 1 < LAYERS) // dz = g (.) act'(z) in place; output layer is linear (no mask)
        {
            for (int i = tid; i < TILE * W_DIM; i += WARPS * 32)
            {
                cur[i] = __hgt(aOut[i], __float2half(0.0F)) ? cur[i] : __float2half(0.0F);
            }
            __syncthreads();
        }

        // dW[layer] += a[layer]^T . dz  (64x64 fp32).  warp owns dW cols [warp*32, warp*32+32) -> 2 col x 4 row fragments.
        {
            wmma::fragment<wmma::accumulator, 16, 16, 16, float> dacc[2][4];
            for (int cb = 0; cb < 2; ++cb) { for (int rb = 0; rb < 4; ++rb) { wmma::fill_fragment(dacc[cb][rb], 0.0F); } }
            for (int kb = 0; kb < TILE / 16; ++kb) // contract over TILE batch rows
            {
                wmma::fragment<wmma::matrix_a, 16, 16, 16, __half, wmma::col_major> af[4]; // a[l]^T: (dW-row k) x (batch)
                for (int rb = 0; rb < 4; ++rb) { wmma::load_matrix_sync(af[rb], aIn + (kb * 16) * W_DIM + rb * 16, W_DIM); }
                for (int cb = 0; cb < 2; ++cb)
                {
                    wmma::fragment<wmma::matrix_b, 16, 16, 16, __half, wmma::row_major> bf; // dz: (batch) x (dW-col n)
                    wmma::load_matrix_sync(bf, cur + (kb * 16) * W_DIM + (warp * 32 + cb * 16), W_DIM);
                    for (int rb = 0; rb < 4; ++rb) { wmma::mma_sync(dacc[cb][rb], af[rb], bf, dacc[cb][rb]); }
                }
            }
            float* dwl = dw + (static_cast<size_t>(blockIdx.x % NGROUP) * LAYERS + layer) * W_DIM * W_DIM;
            for (int cb = 0; cb < 2; ++cb)
            {
                for (int rb = 0; rb < 4; ++rb)
                {
                    wmma::store_matrix_sync(sdw[warp], dacc[cb][rb], 16, wmma::mem_row_major); // sdw[row k, col n]
                    __syncwarp();
#ifndef NOATOMIC
                    for (int e = (tid & 31); e < 256; e += 32)
                    {
                        const int kk = rb * 16 + e / 16;             // dW row k
                        const int nn = warp * 32 + cb * 16 + e % 16; // dW col n
                        atomicAdd(&dwl[kk + 64 * nn], sdw[warp][e]);
                    }
#endif
                    __syncwarp();
                }
            }
        }
        __syncthreads();

        // da = dz . W^T -> new g.  da[r,k] = sum_n dz[r,n]*W[l][k,n] = sum_n dz[r,n]*sw[n*64+k]. A=dz row_major, B[n,k] row_major.
        {
            wmma::fragment<wmma::accumulator, 16, 16, 16, __half> acc[4][4];
            for (int m = 0; m < 4; ++m) { for (int n = 0; n < 4; ++n) { wmma::fill_fragment(acc[m][n], __float2half(0.0F)); } }
            for (int kb = 0; kb < 4; ++kb) // contract over n
            {
                wmma::fragment<wmma::matrix_a, 16, 16, 16, __half, wmma::row_major> af[4];
                for (int m = 0; m < 4; ++m) { wmma::load_matrix_sync(af[m], cur + (warp * 64 + m * 16) * W_DIM + kb * 16, W_DIM); }
                for (int n = 0; n < 4; ++n)
                {
                    wmma::fragment<wmma::matrix_b, 16, 16, 16, __half, wmma::row_major> bf;
                    wmma::load_matrix_sync(bf, sw + (kb * 16) * W_DIM + n * 16, W_DIM); // B[n,k] = sw[n*64+k]
                    for (int m = 0; m < 4; ++m) { wmma::mma_sync(acc[m][n], af[m], bf, acc[m][n]); }
                }
            }
            for (int m = 0; m < 4; ++m)
            {
                for (int n = 0; n < 4; ++n)
                {
                    wmma::store_matrix_sync(nxt + (warp * 64 + m * 16) * W_DIM + n * 16, acc[m][n], W_DIM, wmma::mem_row_major);
                }
            }
        }
        __half* t = cur;
        cur       = nxt;
        nxt       = t;
        __syncthreads();
    }
}

// CPU oracle for dW[L-1] = a[L-1]^T . gout  (dz[L-1] = gout, the linear output layer has no upstream chain).
static void cpu_ref_dw_last(const __half* acts_dev, const __half* gout_dev, float* out64)
{
    __half* a = static_cast<__half*>(malloc(sizeof(__half) * BATCH * W_DIM));
    __half* g = static_cast<__half*>(malloc(sizeof(__half) * BATCH * W_DIM));
    check(cudaMemcpy(a, acts_dev + static_cast<size_t>(LAYERS - 1) * BATCH * W_DIM, sizeof(__half) * BATCH * W_DIM, cudaMemcpyDeviceToHost), "rb a");
    check(cudaMemcpy(g, gout_dev, sizeof(__half) * BATCH * W_DIM, cudaMemcpyDeviceToHost), "rb g");
    for (int i = 0; i < 64 * 64; ++i) { out64[i] = 0.0F; }
    for (int r = 0; r < BATCH; ++r)
    {
        for (int k = 0; k < 64; ++k)
        {
            const float av = __half2float(a[r * 64 + k]);
            for (int n = 0; n < 64; ++n) { out64[k + 64 * n] += av * __half2float(g[r * 64 + n]); }
        }
    }
    free(a); free(g);
}

static double gate_dw(const char* nm, const float* dw_dev, const float* ref)
{
    float* got = static_cast<float*>(malloc(sizeof(float) * 64 * 64));
    check(cudaMemcpy(got, dw_dev + static_cast<size_t>(LAYERS - 1) * W_DIM * W_DIM, sizeof(float) * 64 * 64, cudaMemcpyDeviceToHost), "rb dw");
    double maxad = 0.0, maxmag = 0.0;
    for (int i = 0; i < 64 * 64; ++i)
    {
        const double d = ref[i] > got[i] ? ref[i] - got[i] : got[i] - ref[i];
        if (d > maxad) { maxad = d; }
        const double m = ref[i] < 0 ? -ref[i] : ref[i];
        if (m > maxmag) { maxmag = m; }
    }
    std::printf("# %s dW[L-1]: max_abs_diff=%.3f max_ref_mag=%.3f rel=%.5f\n", nm, maxad, maxmag, maxad / (maxmag > 0 ? maxmag : 1));
    free(got);
    return maxmag > 1e-3 ? maxad / maxmag : 1.0;
}

int main()
{
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# FUSED MLP BACKWARD (wmma, on-chip gradient chain) vs cublasLt backward pipeline — %s\n", prop.name);
    std::printf("# MLP 64 x %d layers, batch %d\n", LAYERS, BATCH);

    unsigned s   = 999983U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<float>(static_cast<int>(s >> 9) - (1 << 22)) / static_cast<float>(1 << 22); };
    __half* h_w  = static_cast<__half*>(malloc(sizeof(__half) * LAYERS * W_DIM * W_DIM));
    for (int i = 0; i < LAYERS * W_DIM * W_DIM; ++i) { h_w[i] = __float2half(rnd() * 0.17F); }
    __half* h_in = static_cast<__half*>(malloc(sizeof(__half) * BATCH * W_DIM));
    for (int i = 0; i < BATCH * W_DIM; ++i) { h_in[i] = __float2half(rnd()); }

    __half *d_w = nullptr, *d_acts = nullptr, *d_gout = nullptr, *d_g = nullptr, *d_dz = nullptr, *d_da = nullptr;
    float * d_dw = nullptr, *d_dw_ref = nullptr;
    check(cudaMalloc(&d_w, sizeof(__half) * LAYERS * W_DIM * W_DIM), "w");
    check(cudaMalloc(&d_acts, sizeof(__half) * (LAYERS + 1) * BATCH * W_DIM), "acts");
    check(cudaMalloc(&d_gout, sizeof(__half) * BATCH * W_DIM), "gout");
    check(cudaMalloc(&d_g, sizeof(__half) * BATCH * W_DIM), "g");
    check(cudaMalloc(&d_dz, sizeof(__half) * BATCH * W_DIM), "dz");
    check(cudaMalloc(&d_da, sizeof(__half) * BATCH * W_DIM), "da");
    check(cudaMalloc(&d_dw, sizeof(float) * LAYERS * W_DIM * W_DIM), "dw");
    check(cudaMalloc(&d_dw_ref, sizeof(float) * LAYERS * W_DIM * W_DIM), "dwref");
    float* d_dw_part = nullptr; // NGROUP partial copies (kill atomic contention)
    check(cudaMalloc(&d_dw_part, sizeof(float) * NGROUP * LDW), "dwpart");
    check(cudaMemcpy(d_w, h_w, sizeof(__half) * LAYERS * W_DIM * W_DIM, cudaMemcpyHostToDevice), "up w");
    check(cudaMemcpy(d_acts, h_in, sizeof(__half) * BATCH * W_DIM, cudaMemcpyHostToDevice), "up a0");

    cublasLtHandle_t lt;
    checkLt(cublasLtCreate(&lt), "create");
    const __half one = __float2half(1.0F), zero = __float2half(0.0F);
    const float  onef = 1.0F, zerof = 0.0F;

    // Give cuBLAS its BEST shot: 256MB workspace + heuristic algo selection (split-K for the huge-K dW reduction needs both).
    void*  d_ws = nullptr;
    size_t ws_sz = static_cast<size_t>(256) << 20;
    check(cudaMalloc(&d_ws, ws_sz), "ws");
    cublasLtMatmulPreference_t pref;
    checkLt(cublasLtMatmulPreferenceCreate(&pref), "pref");
    checkLt(cublasLtMatmulPreferenceSetAttribute(pref, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &ws_sz, sizeof(ws_sz)), "pref ws");
    auto best_algo = [&](cublasLtMatmulDesc_t op, cublasLtMatrixLayout_t a, cublasLtMatrixLayout_t b, cublasLtMatrixLayout_t c) {
        cublasLtMatmulHeuristicResult_t h[8];
        int                             nres = 0;
        checkLt(cublasLtMatmulAlgoGetHeuristic(lt, op, a, b, c, c, pref, 8, h, &nres), "heur");
        if (nres == 0) { std::printf("# WARN: no heuristic algo, using default\n"); }
        return nres > 0 ? h[0].algo : cublasLtMatmulAlgo_t{};
    };

    // FORWARD (cublasLt) storing a[1..L]
    cublasLtMatmulDesc_t opF;
    checkLt(cublasLtMatmulDescCreate(&opF, CUBLAS_COMPUTE_16F, CUDA_R_16F), "descF");
    cublasLtMatrixLayout_t lw, lact;
    checkLt(cublasLtMatrixLayoutCreate(&lw, CUDA_R_16F, W_DIM, W_DIM, W_DIM), "lw");
    checkLt(cublasLtMatrixLayoutCreate(&lact, CUDA_R_16F, W_DIM, BATCH, W_DIM), "lact");
    for (int l = 0; l < LAYERS; ++l)
    {
        const __half*      wl   = d_w + static_cast<size_t>(l) * W_DIM * W_DIM;
        const __half*      aIn  = d_acts + static_cast<size_t>(l) * BATCH * W_DIM;
        __half*            aOut = d_acts + static_cast<size_t>(l + 1) * BATCH * W_DIM;
        cublasLtEpilogue_t e    = (l + 1 < LAYERS) ? CUBLASLT_EPILOGUE_RELU : CUBLASLT_EPILOGUE_DEFAULT;
        checkLt(cublasLtMatmulDescSetAttribute(opF, CUBLASLT_MATMUL_DESC_EPILOGUE, &e, sizeof(e)), "epiF");
        checkLt(cublasLtMatmul(lt, opF, &one, wl, lw, aIn, lact, &zero, aOut, lact, aOut, lact, nullptr, nullptr, 0, nullptr), "fwd");
    }
    check(cudaDeviceSynchronize(), "fwd");
    check(cudaMemcpy(d_gout, d_acts + static_cast<size_t>(LAYERS) * BATCH * W_DIM, sizeof(__half) * BATCH * W_DIM, cudaMemcpyDeviceToDevice), "gout");

    // CPU oracle
    float* ref = static_cast<float*>(malloc(sizeof(float) * 64 * 64));
    cpu_ref_dw_last(d_acts, d_gout, ref);

    // ── FUSED backward ──
    auto fused = [&]() {
        check(cudaMemset(d_dw_part, 0, sizeof(float) * NGROUP * LDW), "z dwpart");
        fused_mlp_bwd<<<BATCH / TILE, WARPS * 32>>>(d_acts, d_w, d_gout, d_dw_part);
        reduce_dw<<<(LDW + 255) / 256, 256>>>(d_dw_part, d_dw);
    };
    fused();
    check(cudaDeviceSynchronize(), "fused");
#ifndef NOATOMIC
    if (gate_dw("FUSED", d_dw, ref) > 0.02) { std::printf("# FUSED MISMATCH\n"); return 2; }
#endif

    // ── cuBLAS backward pipeline ──
    // dW[l] (fp32 64x64) = a[l] . dz^T : C(64,64)=A(64,BATCH)·B^T(BATCH,64), A=a[l] col-major 64xBATCH, B=dz col-major 64xBATCH, transb=T.
    // da[l] (fp16 64xBATCH) = W[l] . dz : mirrors forward (Z=W·A). new g = da.
    cublasLtMatmulDesc_t opDW, opDA;
    checkLt(cublasLtMatmulDescCreate(&opDW, CUBLAS_COMPUTE_32F, CUDA_R_32F), "descDW");
    checkLt(cublasLtMatmulDescCreate(&opDA, CUBLAS_COMPUTE_16F, CUDA_R_16F), "descDA");
    cublasOperation_t opT = CUBLAS_OP_T;
    checkLt(cublasLtMatmulDescSetAttribute(opDW, CUBLASLT_MATMUL_DESC_TRANSB, &opT, sizeof(opT)), "dw tb");
    cublasLtMatrixLayout_t lA_dw, lB_dw, lC_dw;
    checkLt(cublasLtMatrixLayoutCreate(&lA_dw, CUDA_R_16F, W_DIM, BATCH, W_DIM), "lAdw");
    checkLt(cublasLtMatrixLayoutCreate(&lB_dw, CUDA_R_16F, W_DIM, BATCH, W_DIM), "lBdw");
    checkLt(cublasLtMatrixLayoutCreate(&lC_dw, CUDA_R_32F, W_DIM, W_DIM, W_DIM), "lCdw");
    const cublasLtMatmulAlgo_t algo_dw = best_algo(opDW, lA_dw, lB_dw, lC_dw); // split-K for the huge-K reduction, if cuBLAS has it
    const cublasLtMatmulAlgo_t algo_da = best_algo(opDA, lw, lact, lact);
    const int                  msk_blk = 256;
    auto cublas_bwd = [&]() {
        check(cudaMemset(d_dw_ref, 0, sizeof(float) * LAYERS * W_DIM * W_DIM), "z dwref");
        const __half* g = d_gout;
        for (int l = LAYERS - 1; l >= 0; --l)
        {
            const __half* aIn  = d_acts + static_cast<size_t>(l) * BATCH * W_DIM;
            const __half* aOut = d_acts + static_cast<size_t>(l + 1) * BATCH * W_DIM;
            relu_mask<<<(BATCH * W_DIM + msk_blk - 1) / msk_blk, msk_blk>>>(g, aOut, d_dz, BATCH * W_DIM, l + 1 < LAYERS ? 1 : 0);
            float* dwl = d_dw_ref + static_cast<size_t>(l) * W_DIM * W_DIM;
            checkLt(cublasLtMatmul(lt, opDW, &onef, aIn, lA_dw, d_dz, lB_dw, &zerof, dwl, lC_dw, dwl, lC_dw, &algo_dw, d_ws, ws_sz, nullptr), "dW");
            const __half* wl = d_w + static_cast<size_t>(l) * W_DIM * W_DIM;
            checkLt(cublasLtMatmul(lt, opDA, &one, wl, lw, d_dz, lact, &zero, d_da, lact, d_da, lact, &algo_da, d_ws, ws_sz, nullptr), "dA");
            g = d_da;
        }
    };
    cublas_bwd();
    check(cudaDeviceSynchronize(), "cublas bwd");
    if (gate_dw("cuBLAS", d_dw_ref, ref) > 0.02) { std::printf("# cuBLAS MISMATCH\n"); return 2; }

    const auto bench = [&](const char* nm, const auto& fn) {
        for (int wi = 0; wi < 3; ++wi) { fn(); }
        check(cudaDeviceSynchronize(), "warm");
        double best = 1e30;
        for (int r = 0; r < 7; ++r)
        {
            const auto t0 = std::chrono::high_resolution_clock::now();
            for (int it = 0; it < 50; ++it) { fn(); }
            check(cudaDeviceSynchronize(), "sync");
            const auto   t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / 50.0;
            if (ms < best) { best = ms; }
        }
        const double flops = 4.0 * BATCH * W_DIM * W_DIM * LAYERS; // dW + da = 2 GEMMs/layer
        std::printf("%-20s %.5f ms   %.1f TFLOP/s (fp16)\n", nm, best, flops / (best * 1.0e9));
        return best;
    };
    // breakdown: isolate the three cuBLAS backward components to confirm the baseline is honest (which shape dominates?)
    auto only_mask = [&]() {
        const __half* g = d_gout;
        for (int l = LAYERS - 1; l >= 0; --l)
        { relu_mask<<<(BATCH * W_DIM + msk_blk - 1) / msk_blk, msk_blk>>>(g, d_acts + static_cast<size_t>(l + 1) * BATCH * W_DIM, d_dz, BATCH * W_DIM, l + 1 < LAYERS ? 1 : 0); g = d_da; }
    };
    auto only_dw = [&]() {
        for (int l = LAYERS - 1; l >= 0; --l)
        { float* dwl = d_dw_ref + static_cast<size_t>(l) * W_DIM * W_DIM; checkLt(cublasLtMatmul(lt, opDW, &onef, d_acts + static_cast<size_t>(l) * BATCH * W_DIM, lA_dw, d_dz, lB_dw, &zerof, dwl, lC_dw, dwl, lC_dw, &algo_dw, d_ws, ws_sz, nullptr), "dWonly"); }
    };
    auto only_da = [&]() {
        for (int l = LAYERS - 1; l >= 0; --l)
        { checkLt(cublasLtMatmul(lt, opDA, &one, d_w + static_cast<size_t>(l) * W_DIM * W_DIM, lw, d_dz, lact, &zero, d_da, lact, d_da, lact, &algo_da, d_ws, ws_sz, nullptr), "dAonly"); }
    };
    const double t_mask = bench("  cuBLAS mask-only", only_mask);
    const double t_dw   = bench("  cuBLAS dW-only", only_dw);
    const double t_da   = bench("  cuBLAS dA-only", only_da);
    std::printf("# breakdown: mask %.2f + dW %.2f + dA %.2f ms\n", t_mask, t_dw, t_da);
    const double tc = bench("cublasLt-bwd", cublas_bwd);
    const double tf = bench("FUSED-bwd", fused);
    std::printf("# RATIO: fused is %.2fx vs cublasLt (backward)\n", tc / tf);
    return 0;
}
