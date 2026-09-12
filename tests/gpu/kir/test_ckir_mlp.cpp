// test_ckir_mlp.cpp — the CKIR fused-MLP tensor recipe (ckir_mlp.hpp), the NRC moat ported into CKIR. Two tests:
//  (1) the CPU reference oracle (mlp_forward_ref) validated against a hand-computed 2-layer case — proves the emitter's
//      intended math (fp16 GEMM chain + ReLU epilogue + linear output);
//  (2) a hidden tool test ([.emit-cuda-mlp]) that emits the CKIR-authored wmma forward kernel to
//      bench/gpu-compute/ckir_mlp_fwd_gen.cu — the bench driver (ckir_mlp_bench.cu) compiles it with nvcc and duels vs
//      cuBLAS to prove the CKIR-emitted kernel reproduces the 2.41× crush + run-to-run bit-identical determinism. The
//      generated .cu is self-contained (preamble + kernel); it is THE SAME kernel the gold-ref hand-wrote, now from one config.

#include <crd/kir/ckir_cuda.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_mlp.hpp>
#include <crd/kir/ckir_msl.hpp>
#include <crd/kir/ckir_wgsl.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

namespace kir = crd::kir;

namespace
{
// substring search on the emitted kernel source (no std::string in tests)
bool src_has(const crd::containers::String& src, const char* needle)
{
    for (const char* p = src.c_str(); *p != '\0'; ++p)
    {
        const char* a = p;
        const char* b = needle;
        while (*a != '\0' && *b != '\0' && *a == *b)
        {
            ++a;
            ++b;
        }
        if (*b == '\0') { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("CKIR fused-MLP CPU oracle == hand-computed 2-layer MLP", "[kir][mlp]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::MlpConfig             cfg;
    cfg.width      = 16; // one wmma tile wide (oracle is width-agnostic; keep it small + hand-checkable)
    cfg.layers     = 2;  // one hidden ReLU + one linear
    cfg.batch_tile = 16;
    cfg.warps      = 1;
    REQUIRE(cfg.valid());

    const int wd    = cfg.width;
    const int batch = 3;
    const int n_in  = batch * wd;
    const int n_w   = cfg.layers * wd * wd;

    crd::containers::Array<crd::f32> in(&alloc);
    in.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<crd::f32> w(&alloc);
    w.resize(static_cast<crd::usize>(n_w));
    // deterministic small values (kept tiny so ReLU + linear stay exactly representable in the oracle's float math)
    for (int i = 0; i < n_in; ++i) { in[static_cast<crd::usize>(i)] = static_cast<crd::f32>((i % 7) - 3) * 0.25F; }
    for (int i = 0; i < n_w; ++i) { w[static_cast<crd::usize>(i)] = static_cast<crd::f32>((i % 5) - 2) * 0.125F; }

    crd::containers::Array<crd::f32> sa(&alloc);
    crd::containers::Array<crd::f32> sb(&alloc);
    crd::containers::Array<crd::f32> got(&alloc);
    sa.resize(static_cast<crd::usize>(wd));
    sb.resize(static_cast<crd::usize>(wd));
    got.resize(static_cast<crd::usize>(n_in));
    kir::mlp_forward_ref(cfg, in.data(), w.data(), sa.data(), sb.data(), got.data(), batch);

    // independent brute-force reference: z0 = in·W0, a1 = ReLU(z0), out = a1·W1 (W row-major: w[l][k*wd + n] = weight k->n)
    crd::containers::Array<crd::f32> a1(&alloc);
    a1.resize(static_cast<crd::usize>(wd));
    for (int r = 0; r < batch; ++r)
    {
        for (int n = 0; n < wd; ++n)
        {
            crd::f32 z = 0.0F;
            for (int k = 0; k < wd; ++k)
            {
                const int ai = r * wd + k;
                const int wi = k * wd + n;
                z += in[static_cast<crd::usize>(ai)] * w[static_cast<crd::usize>(wi)];
            }
            a1[static_cast<crd::usize>(n)] = z > 0.0F ? z : 0.0F;
        }
        for (int n = 0; n < wd; ++n)
        {
            crd::f32 o = 0.0F;
            for (int k = 0; k < wd; ++k)
            {
                const int wi = wd * wd + k * wd + n;
                o += a1[static_cast<crd::usize>(k)] * w[static_cast<crd::usize>(wi)];
            }
            const int oi = r * wd + n;
            REQUIRE(got[static_cast<crd::usize>(oi)] == o); // bit-identical: same ops, same order
        }
    }
}

TEST_CASE("CKIR fused-MLP FP32 statement-tier graph == CPU oracle (bit-exact)", "[kir][mlp]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::MlpConfig             cfg; // width 64, 6 layers
    cfg.batch_tile = 64;            // irrelevant to the FP32 graph (1 sample/workgroup); kept valid()
    cfg.warps      = 2;
    REQUIRE(cfg.valid());
    const int wd    = cfg.width;
    const int nl    = cfg.layers;
    const int batch = 8;
    const int n_in  = batch * wd;
    const int n_w   = nl * wd * wd;

    kir::KGraph  g(&alloc);
    kir::KEntry  e = kir::build_mlp_fwd_fp32(g, cfg);
    REQUIRE(e.local_size[0] == static_cast<crd::u32>(wd));

    // f32-quantized inputs so both references see identical values
    crd::containers::Array<crd::f64> in_d(&alloc);
    in_d.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<crd::f64> w_d(&alloc);
    w_d.resize(static_cast<crd::usize>(n_w));
    for (int i = 0; i < n_in; ++i) { in_d[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(0.2F * static_cast<float>((i * 7) % 13 - 6))); }
    for (int i = 0; i < n_w; ++i) { w_d[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(0.1F * static_cast<float>((i * 5) % 11 - 5))); }

    crd::containers::Array<crd::f64> out_d(&alloc);
    out_d.resize(static_cast<crd::usize>(n_in));
    kir::KernelBuffer bufs[3];
    bufs[0] = {in_d.data(), n_in, 0, 0};
    bufs[1] = {w_d.data(), n_w, 0, 1};
    bufs[2] = {out_d.data(), n_in, 0, 2};
    kir::eval_cpu_kernel(g, e, bufs, 3, e.local_size[0], &alloc, static_cast<crd::u32>(batch));

    // independent f32 reference
    crd::containers::Array<crd::f32> in_f(&alloc);
    in_f.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<crd::f32> w_f(&alloc);
    w_f.resize(static_cast<crd::usize>(n_w));
    for (int i = 0; i < n_in; ++i) { in_f[static_cast<crd::usize>(i)] = static_cast<crd::f32>(in_d[static_cast<crd::usize>(i)]); }
    for (int i = 0; i < n_w; ++i) { w_f[static_cast<crd::usize>(i)] = static_cast<crd::f32>(w_d[static_cast<crd::usize>(i)]); }
    crd::containers::Array<crd::f32> ref(&alloc);
    ref.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<crd::f32> sa(&alloc);
    crd::containers::Array<crd::f32> sb(&alloc);
    sa.resize(static_cast<crd::usize>(wd));
    sb.resize(static_cast<crd::usize>(wd));
    kir::mlp_forward_ref(cfg, in_f.data(), w_f.data(), sa.data(), sb.data(), ref.data(), batch);

    for (int i = 0; i < n_in; ++i)
    {
        REQUIRE(static_cast<crd::f32>(out_d[static_cast<crd::usize>(i)]) == ref[static_cast<crd::usize>(i)]);
    }
}

TEST_CASE("CKIR fused-MLP BACKWARD (dz chain + DETERMINISTIC dW reduction) == CPU oracle bit-exact", "[kir][mlp]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::MlpConfig             cfg;
    cfg.batch_tile = 64;
    cfg.warps      = 2;
    REQUIRE(cfg.valid());
    const int wd    = cfg.width;
    const int nl    = cfg.layers;
    const int batch = 8;
    const int bw    = batch * wd;
    const int n_a   = (nl + 1) * bw;
    const int n_w   = nl * wd * wd;
    const int n_dz  = nl * bw;
    const int n_dw  = nl * wd * wd;

    // host forward storing ALL activations a_all[(L+1)×batch×W]; gout = a[L] (target 0 ⇒ dL/da[L] = a[L])
    crd::containers::Array<crd::f32> a_all(&alloc);
    a_all.resize(static_cast<crd::usize>(n_a));
    crd::containers::Array<crd::f32> w_f(&alloc);
    w_f.resize(static_cast<crd::usize>(n_w));
    for (int i = 0; i < n_w; ++i) { w_f[static_cast<crd::usize>(i)] = static_cast<crd::f32>(0.1F * static_cast<float>((i * 5) % 11 - 5)); }
    for (int r = 0; r < batch; ++r)
    {
        for (int c = 0; c < wd; ++c)
        {
            const int ai                       = r * wd + c;
            a_all[static_cast<crd::usize>(ai)] = static_cast<crd::f32>(0.2F * static_cast<float>(ai % 13 - 6));
        }
        for (int l = 0; l < nl; ++l)
        {
            for (int n = 0; n < wd; ++n)
            {
                crd::f32 z = 0.0F;
                for (int k = 0; k < wd; ++k)
                {
                    const int ci = l * bw + r * wd + k;
                    const int wi = l * wd * wd + k * wd + n;
                    z += a_all[static_cast<crd::usize>(ci)] * w_f[static_cast<crd::usize>(wi)];
                }
                const int oi                       = (l + 1) * bw + r * wd + n;
                a_all[static_cast<crd::usize>(oi)] = (l + 1 < nl && z < 0.0F) ? 0.0F : z;
            }
        }
    }
    crd::containers::Array<crd::f32> gout(&alloc);
    gout.resize(static_cast<crd::usize>(bw));
    for (int i = 0; i < bw; ++i)
    {
        const int gi                     = nl * bw + i;
        gout[static_cast<crd::usize>(i)] = a_all[static_cast<crd::usize>(gi)];
    }

    // CPU oracle (deterministic reference)
    crd::containers::Array<crd::f32> ref_dz(&alloc);
    ref_dz.resize(static_cast<crd::usize>(n_dz));
    crd::containers::Array<crd::f32> ref_dw(&alloc);
    ref_dw.resize(static_cast<crd::usize>(n_dw));
    crd::containers::Array<crd::f32> gs(&alloc);
    gs.resize(static_cast<crd::usize>(wd));
    crd::containers::Array<crd::f32> ngs(&alloc);
    ngs.resize(static_cast<crd::usize>(wd));
    kir::mlp_backward_ref(cfg, a_all.data(), w_f.data(), gout.data(), batch, ref_dz.data(), ref_dw.data(), gs.data(), ngs.data());

    // f64 buffers for eval_cpu_kernel
    const auto to64 = [&](const crd::containers::Array<crd::f32>& src, crd::containers::Array<crd::f64>& dst) {
        dst.resize(src.size());
        for (crd::usize i = 0; i < src.size(); ++i) { dst[i] = static_cast<crd::f64>(src[i]); }
    };
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<crd::f64> w64(&alloc);
    crd::containers::Array<crd::f64> go64(&alloc);
    to64(a_all, a64);
    to64(w_f, w64);
    to64(gout, go64);

    // Kernel A: dz chain
    kir::KGraph      g_a(&alloc);
    const kir::KEntry e_a = kir::build_mlp_bwd_dz(g_a, cfg, batch);
    crd::containers::Array<crd::f64> dz64(&alloc);
    dz64.resize(static_cast<crd::usize>(n_dz));
    kir::KernelBuffer buf_a[4] = {{a64.data(), n_a, 0, 0}, {w64.data(), n_w, 0, 1}, {go64.data(), bw, 0, 2}, {dz64.data(), n_dz, 0, 3}};
    kir::eval_cpu_kernel(g_a, e_a, buf_a, 4, e_a.local_size[0], &alloc, static_cast<crd::u32>(batch));
    for (int i = 0; i < n_dz; ++i) { REQUIRE(static_cast<crd::f32>(dz64[static_cast<crd::usize>(i)]) == ref_dz[static_cast<crd::usize>(i)]); }

    // Kernel B: DETERMINISTIC dW reduction (uses kernel A's dz_all)
    kir::KGraph      g_b(&alloc);
    const kir::KEntry e_b = kir::build_mlp_bwd_dw(g_b, cfg, batch);
    crd::containers::Array<crd::f64> dw64(&alloc);
    dw64.resize(static_cast<crd::usize>(n_dw));
    kir::KernelBuffer buf_b[3] = {{a64.data(), n_a, 0, 0}, {dz64.data(), n_dz, 0, 1}, {dw64.data(), n_dw, 0, 2}};
    kir::eval_cpu_kernel(g_b, e_b, buf_b, 3, e_b.local_size[0], &alloc, static_cast<crd::u32>(nl * wd));
    for (int i = 0; i < n_dw; ++i) { REQUIRE(static_cast<crd::f32>(dw64[static_cast<crd::usize>(i)]) == ref_dw[static_cast<crd::usize>(i)]); }
}

TEST_CASE("CKIR fused-MLP FP32 forward emits well-formed source for ALL FIVE backends (one builder)", "[kir][mlp]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::MlpConfig             cfg;
    cfg.batch_tile = 64;
    cfg.warps      = 2;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::build_mlp_fwd_fp32(g, cfg);

    // ONE statement-tier graph → every generic backend emitter lowers it. The FP32-precise tier: RUNS + bit-exact on
    // Vulkan + DX12 (verified in the gpu-context tests); CUDA nvcc-compiles; WGSL/MSL emit-validated (no imperative WebGPU
    // context here; no Metal on Windows) — exactly the portability the tensor tier could not reach.
    kir::GlslKernel gl(&alloc);
    kir::GlslKernel hl(&alloc);
    kir::GlslKernel cu(&alloc);
    kir::GlslKernel ms(&alloc);
    kir::GlslKernel wg(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, gl));
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, hl));
    REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, cu));
    REQUIRE(kir::emit_compute_kernel_msl(g, e, &alloc, ms));
    REQUIRE(kir::emit_compute_kernel_wgsl(g, e, &alloc, wg));

    // each backend's compute-shared/barrier landmarks
    CHECK(src_has(gl.source, "shared"));       // GLSL shared memory
    CHECK(src_has(gl.source, "barrier"));
    CHECK(src_has(hl.source, "groupshared"));  // HLSL shared memory
    CHECK(src_has(hl.source, "GroupMemoryBarrierWithGroupSync"));
    CHECK(src_has(cu.source, "__shared__"));   // CUDA shared memory
    CHECK(src_has(cu.source, "__syncthreads"));
    CHECK(src_has(ms.source, "threadgroup"));  // MSL shared memory
    CHECK(src_has(wg.source, "var<workgroup>")); // WGSL shared memory
    CHECK(src_has(wg.source, "workgroupBarrier"));
}

TEST_CASE("CKIR fused-MLP forward emits well-formed CUDA", "[kir][mlp]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::MlpConfig             cfg; // the measured crush config: 64-wide, 6-layer, tile-128, 2-warp
    kir::GlslKernel            k(&alloc);
    REQUIRE(kir::emit_fused_mlp_fwd_cuda(cfg, "fused_mlp", k));
    REQUIRE(k.n_inputs == 3);
    const crd::containers::String& src = k.source;
    // structural sanity: the schedule landmarks must be present (16 wmma accumulator chains/warp = the ILP crush lever)
    const auto has = [&](const char* needle) {
        const char* p = src.c_str();
        for (; *p != '\0'; ++p)
        {
            const char* a = p;
            const char* b = needle;
            while (*a != '\0' && *b != '\0' && *a == *b)
            {
                ++a;
                ++b;
            }
            if (*b == '\0') { return true; }
        }
        return false;
    };
    REQUIRE(has("__global__ void __launch_bounds__(64) fused_mlp"));
    REQUIRE(has("nvcuda::wmma::mma_sync"));
    REQUIRE(has("__hgt("));                     // ReLU epilogue
    REQUIRE(has("acc[4][4]"));                  // 4 row-frags × 4 col-frags
    REQUIRE(has("__shared__ __half sa[8192]")); // tile 128 × width 64
}

// Hidden tool: write the FP32 statement-tier MLP as CUDA for an nvcc compile-validate (proves the generic CUDA emitter
// produces valid CUDA for the portable tier). Kernel entry is `ckir(...)`.
TEST_CASE("emit fused MLP FP32 as CUDA", "[.emit-cuda-mlp-fp32]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::MlpConfig             cfg;
    cfg.batch_tile = 64;
    cfg.warps      = 2;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::build_mlp_fwd_fp32(g, cfg);
    kir::GlslKernel   k(&alloc);
    REQUIRE(kir::emit_compute_kernel_cuda(g, e, &alloc, k));
    std::ofstream f("C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/1487a581-3392-44fb-bc9e-ebeaffd19da5/scratchpad/ckir_mlp_fp32_gen.cu", std::ios::binary);
    REQUIRE(f.is_open());
    f << k.source.c_str();
    f.close();
    CHECK(true);
}

// Hidden tool: write the CKIR-authored tensor BACKWARD kernels (reduce_dw + fused backward) to the bench directory. The
// driver ckir_mlp_bwd_bench.cu compiles + duels cuBLAS to prove the CKIR-emitted backward reproduces the ~1.9x crush.
TEST_CASE("emit fused MLP tensor backward as CUDA", "[.emit-cuda-mlp-bwd]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::MlpConfig             cfg; // width 64, 6 layers, tile 128, 2 warps — the measured backward crush config
    kir::GlslKernel            k(&alloc);
    REQUIRE(kir::emit_fused_mlp_bwd_cuda(cfg, 1 << 20, 8, "ckir_fused_mlp_bwd", k));
    REQUIRE(k.n_inputs == 4);
    std::ofstream f("D:/Dev/cerid/bench/gpu-compute/ckir_mlp_bwd_gen.cu", std::ios::binary);
    REQUIRE(f.is_open());
    f << "// GENERATED by test_ckir_mlp.cpp [.emit-cuda-mlp-bwd] -- CKIR fused-MLP tensor backward, lowered to CUDA wmma.\n";
    f << "// Config: width 64, 6 layers, tile 128, 2 warps, batch 1M, NGROUP 8 -- the measured ~1.9x backward crush.\n";
    f << "#include <cuda_fp16.h>\n#include <mma.h>\n\n";
    f << k.source.c_str();
    f.close();
    CHECK(true);
}

// Hidden tool: write the CKIR-authored forward kernel (preamble + kernel) to the bench directory.
TEST_CASE("emit fused MLP forward as CUDA", "[.emit-cuda-mlp]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::MlpConfig             cfg;
    kir::GlslKernel            k(&alloc);
    REQUIRE(kir::emit_fused_mlp_fwd_cuda(cfg, "ckir_fused_mlp_fwd", k));

    std::ofstream f("D:/Dev/cerid/bench/gpu-compute/ckir_mlp_fwd_gen.cu", std::ios::binary);
    REQUIRE(f.is_open());
    f << "// GENERATED by test_ckir_mlp.cpp [.emit-cuda-mlp] -- CKIR fused-MLP forward, lowered to CUDA wmma.\n";
    f << "// Config: width 64, 6 layers, tile 128, 2 warps -- the measured 2.4x crush vs cuBLAS.\n";
    f << "#include <cuda_fp16.h>\n#include <mma.h>\n\n";
    f << k.source.c_str();
    f.close();
    CHECK(true);
}
