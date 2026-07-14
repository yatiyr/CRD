// mlp_fused_bench.cu — THE NRC MOAT DUEL: a FULLY-FUSED 64-wide MLP (tensor cores, activations never leave the chip) vs
// NVIDIA's strongest vendor pipeline (cublasLt fp16 GEMMs with fused ReLU epilogues, activations round-tripping DRAM between
// layers). The structural thesis (the FFT-crush fusion doctrine): L layers of N x 64 activations cost cuBLAS ~2*L*N*64*2 bytes
// of DRAM traffic; the fused kernel reads the input once and writes the output once (~2*N*64*2) => ~L x less traffic.
// MLP: in 64 -> 5 x hidden 64 (ReLU) -> out 64 (linear) = 6 weight mats 64x64, fp16, fp16 accumulate (both sides identical).
// Build: nvcc -O3 -std=c++17 -arch=sm_89 -allow-unsupported-compiler -Xcompiler /Zc:preprocessor
//        mlp_fused_bench.cu -o mlp_fused_bench.exe -lcublasLt -lcublas
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

constexpr int W_DIM   = 64;      // MLP width
constexpr int LAYERS  = 6;       // weight matrices (5 hidden ReLU + 1 linear out)
constexpr int BATCH   = 1 << 20; // 1M samples
constexpr int TILE    = 128;     // batch rows per block
constexpr int WARPS   = 2;       // each warp owns 64 rows (4 row-fragments: 16 independent mma chains)

// ── THE FUSED MLP ─────────────────────────────────────────────────────────────────────────────────────────────────────
// One block = 64 samples through ALL layers. Activations ping-pong in shared (64x64 fp16 = 8KB x2); the current layer's
// weights are staged into shared (8KB). Each warp computes a 16-row stripe as 4 wmma 16x16x16 fragments (fp16 accumulate,
// fixed schedule => deterministic). ReLU applied on the accumulator fragments in registers. DRAM: input read once, output
// written once. Weights come from L2 (48KB total, shared by every block).
extern "C" __global__ void __launch_bounds__(WARPS * 32) fused_mlp(const __half* __restrict__ in,
                                                                   const __half* __restrict__ w, __half* __restrict__ out,
                                                                   int nrows)
{
    __shared__ __half sa[TILE * W_DIM]; // activations (ping)
    __shared__ __half sb[TILE * W_DIM]; // activations (pong)
    __shared__ __half sw[W_DIM * W_DIM];

    const int block_row = blockIdx.x * TILE;
    const int tid       = threadIdx.x;
    const int warp      = tid / 32;

    // load the input tile (coalesced): TILE rows x 64 cols fp16, all threads float4-strided
    {
        const __half* src = in + static_cast<size_t>(block_row) * W_DIM;
        for (int i = tid; i < TILE * W_DIM / 8; i += WARPS * 32)
        {
            reinterpret_cast<float4*>(sa)[i] = reinterpret_cast<const float4*>(src)[i];
        }
    }

    __half* cur = sa;
    __half* nxt = sb;
    for (int layer = 0; layer < LAYERS; ++layer)
    {
        // stage this layer's weights in shared (col-major 64x64: w[k + 64*n] = W[n][k]); shared beats L2-direct fragment reads
        const __half* wl = w + static_cast<size_t>(layer) * W_DIM * W_DIM;
        for (int i = tid; i < W_DIM * W_DIM / 8; i += WARPS * 32)
        {
            reinterpret_cast<float4*>(sw)[i] = reinterpret_cast<const float4*>(wl)[i];
        }
        __syncthreads(); // sw visible + (from layer 1) prev-layer nxt writes visible before this layer reads cur

        // warp stripe: rows [warp*64, warp*64+64) as 4 row-fragments x 4 col-fragments = 16 independent accumulators (ILP)
        wmma::fragment<wmma::accumulator, 16, 16, 16, __half> acc[4][4];
        for (int m = 0; m < 4; ++m) { for (int n = 0; n < 4; ++n) { wmma::fill_fragment(acc[m][n], __float2half(0.0F)); } }
        for (int k = 0; k < 4; ++k)
        {
            wmma::fragment<wmma::matrix_a, 16, 16, 16, __half, wmma::row_major> af[4];
            for (int m = 0; m < 4; ++m) { wmma::load_matrix_sync(af[m], cur + (warp * 64 + m * 16) * W_DIM + k * 16, W_DIM); }
            for (int n = 0; n < 4; ++n)
            {
                wmma::fragment<wmma::matrix_b, 16, 16, 16, __half, wmma::row_major> bf;
                wmma::load_matrix_sync(bf, sw + (k * 16) * W_DIM + n * 16, W_DIM); // B[k][n] = w[64k + n] == cuBLAS col-major W
                for (int m = 0; m < 4; ++m) { wmma::mma_sync(acc[m][n], af[m], bf, acc[m][n]); }
            }
        }
        if (layer + 1 < LAYERS) // hidden layers: ReLU in registers
        {
            for (int m = 0; m < 4; ++m)
            {
                for (int n = 0; n < 4; ++n)
                {
                    for (int e = 0; e < acc[m][n].num_elements; ++e)
                    {
                        acc[m][n].x[e] = __hgt(acc[m][n].x[e], __float2half(0.0F)) ? acc[m][n].x[e] : __float2half(0.0F);
                    }
                }
            }
        }
        // ping-pong store into the OTHER buffer -- no read/write aliasing of cur
        for (int m = 0; m < 4; ++m)
        {
            for (int n = 0; n < 4; ++n)
            {
                wmma::store_matrix_sync(nxt + (warp * 64 + m * 16) * W_DIM + n * 16, acc[m][n], W_DIM, wmma::mem_row_major);
            }
        }
        __half* t = cur;
        cur       = nxt;
        nxt       = t;
        __syncthreads(); // new activations visible before next layer + sw reuse
    }

    // write the output tile (coalesced)
    {
        __half* dst = out + static_cast<size_t>(block_row) * W_DIM;
        for (int i = tid; i < TILE * W_DIM / 8; i += WARPS * 32)
        {
            reinterpret_cast<float4*>(dst)[i] = reinterpret_cast<const float4*>(cur)[i];
        }
    }
    (void)nrows;
}

int main()
{
    cudaDeviceProp prop{};
    cudaGetDeviceProperties(&prop, 0);
    std::printf("# FUSED MLP (wmma fp16, activations on-chip) vs cublasLt fp16+ReLU-epilogue — %s\n", prop.name);
    std::printf("# MLP 64 x %d layers, batch %d\n", LAYERS, BATCH);

    // host data: deterministic LCG weights ~ U(-0.17, 0.17) (keeps 6-layer fp16 activations in range), input U(-1,1)
    __half* h_in = static_cast<__half*>(malloc(sizeof(__half) * BATCH * W_DIM));
    __half* h_w  = static_cast<__half*>(malloc(sizeof(__half) * LAYERS * W_DIM * W_DIM));
    unsigned s   = 12345U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<float>(static_cast<int>(s >> 9) - (1 << 22)) / static_cast<float>(1 << 22); };
    for (int i = 0; i < BATCH * W_DIM; ++i) { h_in[i] = __float2half(rnd()); }
    for (int i = 0; i < LAYERS * W_DIM * W_DIM; ++i) { h_w[i] = __float2half(rnd() * 0.17F); }

    __half *d_in = nullptr, *d_w = nullptr, *d_out = nullptr, *d_a = nullptr, *d_b = nullptr;
    check(cudaMalloc(&d_in, sizeof(__half) * BATCH * W_DIM), "in");
    check(cudaMalloc(&d_w, sizeof(__half) * LAYERS * W_DIM * W_DIM), "w");
    check(cudaMalloc(&d_out, sizeof(__half) * BATCH * W_DIM), "out");
    check(cudaMalloc(&d_a, sizeof(__half) * BATCH * W_DIM), "acta");
    check(cudaMalloc(&d_b, sizeof(__half) * BATCH * W_DIM), "actb");
    check(cudaMemcpy(d_in, h_in, sizeof(__half) * BATCH * W_DIM, cudaMemcpyHostToDevice), "up in");
    check(cudaMemcpy(d_w, h_w, sizeof(__half) * LAYERS * W_DIM * W_DIM, cudaMemcpyHostToDevice), "up w");

    // ── cublasLt pipeline: 6 x [C(64,N) = W(64,64) x A(64,N)] col-major, fp16, COMPUTE_16F, ReLU epilogue on hidden ──
    cublasLtHandle_t lt;
    checkLt(cublasLtCreate(&lt), "create");
    cublasLtMatmulDesc_t opRelu, opLin;
    checkLt(cublasLtMatmulDescCreate(&opRelu, CUBLAS_COMPUTE_16F, CUDA_R_16F), "desc");
    checkLt(cublasLtMatmulDescCreate(&opLin, CUBLAS_COMPUTE_16F, CUDA_R_16F), "desc2");
    cublasLtEpilogue_t epi = CUBLASLT_EPILOGUE_RELU;
    checkLt(cublasLtMatmulDescSetAttribute(opRelu, CUBLASLT_MATMUL_DESC_EPILOGUE, &epi, sizeof(epi)), "epi");
    cublasLtMatrixLayout_t lw, la, lc;
    checkLt(cublasLtMatrixLayoutCreate(&lw, CUDA_R_16F, W_DIM, W_DIM, W_DIM), "lw");     // W: 64x64
    checkLt(cublasLtMatrixLayoutCreate(&la, CUDA_R_16F, W_DIM, BATCH, W_DIM), "la");     // A: 64xN
    checkLt(cublasLtMatrixLayoutCreate(&lc, CUDA_R_16F, W_DIM, BATCH, W_DIM), "lc");     // C: 64xN
    const __half one  = __float2half(1.0F);
    const __half zero = __float2half(0.0F);
    const auto   cublas_mlp = [&]() {
        const __half* src = d_in;
        __half*       pa  = d_a;
        __half*       pb  = d_b;
        for (int l = 0; l < LAYERS; ++l)
        {
            const __half* wl = d_w + static_cast<size_t>(l) * W_DIM * W_DIM;
            __half*       dst = (l + 1 == LAYERS) ? d_out : pa;
            checkLt(cublasLtMatmul(lt, (l + 1 == LAYERS) ? opLin : opRelu, &one, wl, lw, src, la, &zero, dst, lc, dst, lc,
                                   nullptr, nullptr, 0, nullptr),
                    "matmul");
            src = dst;
            __half* t = pa; pa = pb; pb = t;
        }
    };

    const auto fused = [&]() { fused_mlp<<<BATCH / TILE, WARPS * 32>>>(d_in, d_w, d_out, BATCH); };

    // correctness: fused vs cublas (same fp16 math, different accumulation order => tolerance)
    __half* h_ref = static_cast<__half*>(malloc(sizeof(__half) * BATCH * W_DIM));
    __half* h_got = static_cast<__half*>(malloc(sizeof(__half) * BATCH * W_DIM));
    cublas_mlp();
    check(cudaDeviceSynchronize(), "cublas first");
    check(cudaMemcpy(h_ref, d_out, sizeof(__half) * BATCH * W_DIM, cudaMemcpyDeviceToHost), "ref rb");
    fused();
    check(cudaDeviceSynchronize(), "fused first");
    check(cudaMemcpy(h_got, d_out, sizeof(__half) * BATCH * W_DIM, cudaMemcpyDeviceToHost), "got rb");
    {
        double maxad = 0.0, maxmag = 0.0;
        for (int i = 0; i < BATCH * W_DIM; ++i)
        {
            const double r = __half2float(h_ref[i]);
            const double g = __half2float(h_got[i]);
            const double d = r > g ? r - g : g - r;
            if (d > maxad) { maxad = d; }
            const double m = r < 0 ? -r : r;
            if (m > maxmag) { maxmag = m; }
        }
        std::printf("# correctness: max_abs_diff=%.5f max_ref_mag=%.5f rel=%.5f\n", maxad, maxmag, maxad / (maxmag > 0 ? maxmag : 1));
        if (maxmag < 1e-3 || maxad / maxmag > 0.08) { std::printf("# MISMATCH\n"); return 2; }
    }

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
        const double flops = 2.0 * BATCH * W_DIM * W_DIM * LAYERS;
        std::printf("%-18s %.5f ms   %.1f TFLOP/s (fp16)\n", nm, best, flops / (best * 1.0e9));
        return best;
    };
    const double tc = bench("cublasLt-MLP", cublas_mlp);
    const double tf = bench("FUSED-MLP", fused);
    std::printf("# RATIO: fused is %.2fx vs cublasLt\n", tc / tf);
    return 0;
}
