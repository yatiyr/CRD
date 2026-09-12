// CEIR-22c-3b (device-free) — the AUTHORED viz-prep .ckir kernels' ORACLE. ckir_read the two hand-authored files
// (assets/ckir/tensor_viz_{magnitude,normalize}.ckir) and eval_cpu_kernel them over L=64 elements, proving the AUTHORING is
// correct (the node/stmt graph computes mag=sqrt(re^2+im^2) and norm=mag/max) INDEPENDENT of any device — the first gate that
// fails if the positional refs / Builtin gid / dtype grammar are wrong. Also smoke emit_compute_kernel_glsl — which returns true
// once a STRING is produced, NOT a compile check (the shaderc compiler lives in the Vulkan backend per ADR-0103; kir can't link
// it), so scope bugs pass here and only the device legs' compile catches them. One workgroup of local_size=L maps 1:1 onto the
// elements (no tail threads → no OOB).

#include <crd/kir/ckir.hpp>             // KGraph / KEntry
#include <crd/kir/ckir_asset.hpp>       // ckir_read
#include <crd/kir/ckir_glsl.hpp>        // emit_compute_kernel_glsl
#include <crd/kir/ckir_hlsl.hpp>        // CEIR-24b-2: emit_compute_kernel_hlsl (the softmax Exp DX12-leg emit smoke)
#include <crd/kir/ckir_kernel_eval.hpp> // eval_cpu_kernel / KernelBuffer

#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace kir = crd::kir;

namespace
{
constexpr int kL = 64;

// Read + parse an authored .ckir into (kg, ke). REQUIRE-fails loudly on a bad path / malformed grammar (the authoring gate).
void load_ckir(const char* path, crd::memory::IAllocator* alloc, kir::KGraph& kg, kir::KEntry& ke)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    crd::containers::Array<char> src(alloc);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    const kir::CkirReadResult r = kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), kg, ke);
    REQUIRE(r.ok);
}
double absd(double x) { return x < 0.0 ? -x : x; }
} // namespace

TEST_CASE("ceir 22c-3b: the authored tensor_viz_magnitude.ckir computes sqrt(re^2+im^2) per element", "[ceir][ckir][viz]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/tensor_viz_magnitude.ckir", &root, kg, ke);

    // emit smoke — the device legs compile this exact GLSL.
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern));

    crd::f64 re[kL];
    crd::f64 im[kL];
    crd::f64 mag[kL];
    for (int i = 0; i < kL; ++i)
    {
        re[i]  = static_cast<crd::f64>((i % 7) - 3);
        im[i]  = static_cast<crd::f64>((i % 5) - 2);
        mag[i] = -1.0; // poison — the kernel must overwrite every element
    }
    kir::KernelBuffer bufs[3] = {{re, kL, 0, 0}, {im, kL, 0, 1}, {mag, kL, 0, 2}};
    kir::eval_cpu_kernel(kg, ke, bufs, 3, static_cast<crd::u32>(kL), &root, 1U);

    for (int i = 0; i < kL; ++i)
    {
        const crd::f64 ref = crd::math::sqrt(re[i] * re[i] + im[i] * im[i]);
        // eval_cpu_kernel rounds the store to the buffer's F32 dtype (it MODELS the f32 device kernel), so an f64 reference
        // differs by ~1 f32 ulp — a RELATIVE f32 tolerance (DERIVED from the store precision, not a tuned epsilon).
        CHECK(absd(mag[i] - ref) <= 1e-6 * (1.0 + absd(ref)));
    }
}

TEST_CASE("ceir 22c-3b: the authored tensor_viz_normalize.ckir computes mag/max per element (rank-0 scalar read)", "[ceir][ckir][viz]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/tensor_viz_normalize.ckir", &root, kg, ke);

    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern));

    crd::f64 mag[kL];
    crd::f64 norm[kL];
    crd::f64 mx[1];
    crd::f64 peak = 0.0;
    for (int i = 0; i < kL; ++i)
    {
        mag[i]  = static_cast<crd::f64>(i % 11) + 0.5; // strictly positive, a clear peak at i%11==10
        norm[i] = -1.0;
        peak    = peak > mag[i] ? peak : mag[i];
    }
    mx[0] = peak;
    kir::KernelBuffer bufs[3] = {{mag, kL, 0, 0}, {mx, 1, 0, 1}, {norm, kL, 0, 2}};
    kir::eval_cpu_kernel(kg, ke, bufs, 3, static_cast<crd::u32>(kL), &root, 1U);

    for (int i = 0; i < kL; ++i)
    {
        const crd::f64 ref = mag[i] / peak;
        CHECK(absd(norm[i] - ref) <= 1e-6 * (1.0 + absd(ref))); // f32-store precision (as magnitude)
    }
    // the normalized peak is 1.0 within f32 (max/max = 1.0; the display invariant).
    crd::f64 nmax = 0.0;
    for (int i = 0; i < kL; ++i) { nmax = nmax > norm[i] ? nmax : norm[i]; }
    CHECK(absd(nmax - 1.0) < 1e-6);
}

TEST_CASE("ceir 23b-1: the authored quant_dequantize_q8.ckir dequantizes u32-packed int8 (sign-extend) vs the CPU ref",
          "[ceir][ckir][quant]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/quant_dequantize_q8.ckir", &root, kg, ke);
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern)); // emit smoke (the device legs compile this)

    constexpr int nq = 64; // N % 4 == 0
    crd::i32      b[nq];
    for (int i = 0; i < nq; ++i) { b[i] = ((i * 37 + 11) % 256) - 128; } // int8 values spanning [-128,127]
    crd::f64 packed[nq / 4];
    for (int w = 0; w < nq / 4; ++w)
    {
        crd::u32 word = 0;
        for (int j = 0; j < 4; ++j) { word |= static_cast<crd::u32>(b[4 * w + j] & 0xFF) << (8U * static_cast<crd::u32>(j)); }
        packed[w] = static_cast<crd::f64>(word); // u32 ≤ 2^32 < 2^53 ⇒ exact in f64
    }
    crd::f64 scale[1] = {0.5};
    crd::f64 zp[1]    = {3.0};
    crd::f64 out[nq];
    for (int i = 0; i < nq; ++i) { out[i] = -999.0; }
    kir::KernelBuffer bufs[4] = {{packed, nq / 4, 0, 0}, {scale, 1, 0, 1}, {zp, 1, 0, 2}, {out, nq, 0, 3}};
    kir::eval_cpu_kernel(kg, ke, bufs, 4, static_cast<crd::u32>(nq), &root, 1U);

    for (int i = 0; i < nq; ++i)
    {
        const crd::f64 ref = (static_cast<crd::f64>(b[i]) - zp[0]) * scale[0]; // (int8 - zp) * scale
        CHECK(absd(out[i] - ref) <= 1e-6 * (1.0 + absd(ref)));
    }
}

TEST_CASE("ceir 23b-2c: the fused quant_gemm_q8.ckir (dequant-inline gemm) vs the float dequant-then-gemm ref",
          "[ceir][ckir][quant]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/quant_gemm_q8.ckir", &root, kg, ke);
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern)); // emit smoke (the device legs compile this fused kernel)

    constexpr int rows  = 4; // M — the kernel is baked at these dims (M*N=32 threads, K unrolled, K*N=64)
    constexpr int inner = 8; // K
    constexpr int cols  = 8; // N
    crd::f64      a_in[rows * inner];
    for (int i = 0; i < rows * inner; ++i) { a_in[i] = 0.25 * static_cast<crd::f64>((i * 13 + 5) % 9 - 4); } // small floats in [-1,1]
    crd::i32 wq[inner * cols];
    for (int i = 0; i < inner * cols; ++i) { wq[i] = ((i * 37 + 11) % 256) - 128; } // int8 weights spanning [-128,127]
    crd::f64 packed[inner * cols / 4];
    for (int w = 0; w < inner * cols / 4; ++w)
    {
        crd::u32 word = 0;
        for (int j = 0; j < 4; ++j) { word |= static_cast<crd::u32>(wq[4 * w + j] & 0xFF) << (8U * static_cast<crd::u32>(j)); }
        packed[w] = static_cast<crd::f64>(word); // u32 < 2^53 ⇒ exact in f64
    }
    crd::f64 scale[1] = {0.125};
    crd::f64 d_out[rows * cols];
    for (int i = 0; i < rows * cols; ++i) { d_out[i] = -999.0; }
    kir::KernelBuffer bufs[4] = {{a_in, rows * inner, 0, 0}, {packed, inner * cols / 4, 0, 1}, {scale, 1, 0, 2}, {d_out, rows * cols, 0, 3}};
    kir::eval_cpu_kernel(kg, ke, bufs, 4, static_cast<crd::u32>(rows * cols), &root, 1U);

    for (int m = 0; m < rows; ++m)
    {
        for (int n = 0; n < cols; ++n)
        {
            crd::f64 acc = 0.0;
            for (int k = 0; k < inner; ++k) { acc += a_in[m * inner + k] * static_cast<crd::f64>(wq[k * cols + n]); } // D=scale*A·int8(W)
            const crd::f64 ref = acc * scale[0];
            CHECK(absd(d_out[m * cols + n] - ref) <= 1e-4 * (1.0 + absd(ref)));
        }
    }
}

TEST_CASE("ceir 23c-a: the authored relu.ckir computes max(x, 0) per element (the quant-MLP activation)", "[ceir][ckir][quant]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/relu.ckir", &root, kg, ke);
    // CEIR-26d: relu.ckir now ships the SENTINEL local_size=0 ("bind from numel at cook"); the device legs' resolver patches it
    // (bind_authored_local_size). This CPU gate is not a resolver, so bind it explicitly to this corpus's width (32) before the
    // emit smoke so the emitted GLSL is representative of what the device legs compile. eval_cpu_kernel below takes len directly.
    ke.local_size[0] = 32;
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern)); // emit smoke (the device legs compile this)

    constexpr int len = 32; // the MLP hidden width h1[4,8]
    crd::f64      in[len];
    for (int i = 0; i < len; ++i) { in[i] = static_cast<crd::f64>(i - 16) * 0.5; } // span negatives and positives
    crd::f64 out[len];
    for (int i = 0; i < len; ++i) { out[i] = -999.0; }
    kir::KernelBuffer bufs[2] = {{in, len, 0, 0}, {out, len, 0, 1}};
    kir::eval_cpu_kernel(kg, ke, bufs, 2, static_cast<crd::u32>(len), &root, 1U);
    for (int i = 0; i < len; ++i)
    {
        const crd::f64 ref = in[i] > 0.0 ? in[i] : 0.0;
        CHECK(absd(out[i] - ref) <= 1e-6 * (1.0 + absd(ref)));
    }
}

TEST_CASE("ceir 23e-a: the authored spmv_csr.ckir computes CSR y = A*x (runtime For + ForBreakIf + col_idx) vs the CPU ref",
          "[ceir][ckir][sparse]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/spmv_csr.ckir", &root, kg, ke);
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern)); // emit smoke (the device legs compile this)

    // a 4x4 CSR matrix, row lengths 2,3,1,2 (nnz=8, max row length 3 == the kernel's baked bound), N=4.
    constexpr int rows    = 4;
    crd::f64      rp[5]    = {0.0, 2.0, 5.0, 6.0, 8.0};                     // row_ptr (u32, stored as exact f64)
    crd::f64      ci[8]    = {0.0, 1.0, 0.0, 2.0, 3.0, 1.0, 2.0, 3.0};      // col_idx (u32)
    crd::f64      vl[8]    = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};      // values (f32)
    crd::f64      xv[4]    = {2.0, 3.0, 5.0, 7.0};                          // x (f32)
    crd::f64      y[rows]  = {-1.0, -1.0, -1.0, -1.0};
    kir::KernelBuffer bufs[5] = {{rp, 5, 0, 0}, {ci, 8, 0, 1}, {vl, 8, 0, 2}, {xv, 4, 0, 3}, {y, rows, 0, 4}};
    kir::eval_cpu_kernel(kg, ke, bufs, 5, static_cast<crd::u32>(rows), &root, 1U);

    for (int i = 0; i < rows; ++i)
    {
        crd::f64 acc = 0.0;
        for (int k = static_cast<int>(rp[i]); k < static_cast<int>(rp[i + 1]); ++k)
        {
            acc += vl[k] * xv[static_cast<int>(ci[k])]; // y[i] = sum values[k]*x[col_idx[k]]
        }
        CHECK(y[i] == acc); // small integer products -> exact in f32
    }
}

// ⛔ CEIR-26d-3c: the "ceir 24b-1" transpose.ckir reading gate is RETIRED with its asset — attention's Kᵀ is now a shape-generic
// tensor.transpose (a DIALECT op lowered by synth_transpose/emit_permute, the cooker, since 26d-3a), so the baked transpose.ckir
// authored asset is redundant and DELETED (the DELETION-IS-done discipline). Transpose is proven device-resident by synth at
// generic dims via the 26d-3c attention gates (test_ceir_pipeline_{vulkan,dx12}.cpp) + the 25b-4a transpose+broadcast+elementwise chain gate.

// CEIR-24b-2 — the authored softmax.ckir reading gate: ckir_read the committed asset -> eval_cpu_kernel == the CPU scaled
// rowwise softmax (probs[r,c] = exp(scale·scores[r,c] - m_r)/Σ, m_r = max_c scale·scores[r,c]) within a DERIVED tol (exp is
// float math, bit-exactness dies) + GLSL AND HLSL emit smoke (KOp::Exp in both backends; the emitter-lag scar). The durable
// proof of the attention softmax step (the bootstrap builder in test_ckir_kernel.cpp was DELETED). scale = 1/√D is a
// CALLER-UPLOADED 1-element buffer. Dims Sq=2, Sk=3; local_size = Sq = 2, one workgroup.
// CEIR-24b-2 / 26d-3b: the authored softmax.ckir READING GATE — ckir_read the COMMITTED asset → eval_cpu_kernel == the CPU scaled
// rowwise softmax at BOTH the 24b proof dims (Sq=2,Sk=3) AND a GENERIC (Sq=3,Sk=5) width (26d-3b generalized the baked Sk=3 unroll
// to a spec-const loop). Sk rides as the spec-const (constant_id 0, set per case); local_size = Sq (the resolver cook-binds it to
// dim0; the gate sets it). Derived tol (exp is float, not bit-exact). Proves the COMMITTED asset (the builder is DELETED).
TEST_CASE("ceir 24b-2: the authored softmax.ckir computes scaled rowwise softmax at (2,3) AND (3,5) vs the CPU ref (26d-3b generalized)",
          "[ceir][ckir][ml]")
{
    crd::memory::GrowableTlsfAllocator root;
    const auto                         run_case = [&](int sq, int sk, bool emit_smoke) {
        kir::KGraph kg(&root);
        kir::KEntry ke;
        load_ckir(CRD_REPO_DIR "/assets/ckir/softmax.ckir", &root, kg, ke);
        ke.local_size[0] = static_cast<crd::u32>(sq);          // the resolver cook-binds local_size ← Sq=dim0; the gate sets it
        (void)kg.set_spec_const(0U, static_cast<crd::f64>(sk)); // Sk = the spec-const loop bound
        if (emit_smoke)
        {
            kir::GlslKernel gk(&root);
            REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, gk)); // Vulkan/lavapipe leg
            kir::GlslKernel hk(&root);
            REQUIRE(kir::emit_compute_kernel_hlsl(kg, ke, &root, hk)); // DX12 leg (Exp in HLSL)
        }
        const crd::f64 scale_v = 0.5; // 1/√D stand-in
        const int      n       = sq * sk;
        crd::f64       sc[3 * 5];
        crd::f64       sv[1] = {scale_v};
        crd::f64       pr[3 * 5];
        for (int i = 0; i < n; ++i) { sc[i] = 0.3 * (static_cast<crd::f64>(i) - static_cast<crd::f64>(n) * 0.5); }
        for (int i = 0; i < n; ++i) { pr[i] = -1.0; }
        kir::KernelBuffer bufs[3] = {{sc, n, 0, 0}, {sv, 1, 0, 1}, {pr, n, 0, 2}};
        kir::eval_cpu_kernel(kg, ke, bufs, 3, static_cast<crd::u32>(sq), &root, 1U);
        for (int r = 0; r < sq; ++r)
        {
            crd::f64 m = -1e30;
            for (int c = 0; c < sk; ++c) { m = crd::math::max(m, scale_v * sc[r * sk + c]); }
            crd::f64 dn = 0.0;
            for (int c = 0; c < sk; ++c) { dn += crd::math::exp(scale_v * sc[r * sk + c] - m); }
            for (int c = 0; c < sk; ++c)
            {
                const crd::f64 ref = crd::math::exp(scale_v * sc[r * sk + c] - m) / dn;
                CHECK(crd::math::abs(pr[r * sk + c] - ref) <= 1e-6 * (1.0 + crd::math::abs(ref))); // derived tol (exp != bit-exact)
            }
        }
    };
    // emit_compute_kernel_glsl/_hlsl return true once a STRING is produced — they do NOT compile it, so a scope bug (an
    // out-of-scope loop temp) passes this "smoke" and only the compile catches it. The COMPILE authority is the 24b-4 device
    // gates (glslang for Vk, dxc for DX12; ADR-0103 moved the shaderc compiler into the Vulkan backend, out of kir — so this
    // device-free gate CANNOT link it). 26d-3b's materialize bug proved this: emit smoke + CPU eval were both green; only the
    // device compile rejected the out-of-scope `base` (fixed via stmt_materialize). This gate's job is the CPU-eval oracle.
    run_case(2, 3, true);  // the 24b proof dims + the GLSL/HLSL emit string-smoke (NOT a compile — the 24b-4 device gates compile+run it)
    run_case(3, 5, false); // a GENERIC width — the 26d-3b dimension-general proof (was BakedKernelShapeUnsupported)
}

TEST_CASE("ceir 23z: the authored quant_dequantize_q8_sym.ckir (symmetric, out = int8*scale) reads + evals vs the CPU ref",
          "[ceir][ckir][quant]")
{
    crd::memory::GrowableTlsfAllocator root;
    kir::KGraph                        kg(&root);
    kir::KEntry                        ke;
    load_ckir(CRD_REPO_DIR "/assets/ckir/quant_dequantize_q8_sym.ckir", &root, kg, ke); // the 23b-2a symmetric dequant kernel
    kir::GlslKernel kern(&root);
    REQUIRE(kir::emit_compute_kernel_glsl(kg, ke, &root, kern));

    constexpr int len = 64; // len % 4 == 0
    crd::i32      q[len];
    for (int i = 0; i < len; ++i) { q[i] = ((i * 37 + 11) % 256) - 128; } // int8 values spanning [-128,127]
    crd::f64 packed[len / 4];
    for (int w = 0; w < len / 4; ++w)
    {
        crd::u32 word = 0;
        for (int j = 0; j < 4; ++j) { word |= static_cast<crd::u32>(q[4 * w + j] & 0xFF) << (8U * static_cast<crd::u32>(j)); }
        packed[w] = static_cast<crd::f64>(word);
    }
    crd::f64 scale[1] = {0.5};
    crd::f64 out[len];
    for (int i = 0; i < len; ++i) { out[i] = -999.0; }
    kir::KernelBuffer bufs[3] = {{packed, len / 4, 0, 0}, {scale, 1, 0, 1}, {out, len, 0, 2}};
    kir::eval_cpu_kernel(kg, ke, bufs, 3, static_cast<crd::u32>(len), &root, 1U);

    for (int i = 0; i < len; ++i)
    {
        const crd::f64 ref = static_cast<crd::f64>(q[i]) * scale[0]; // SYMMETRIC: int8 * scale (no zero_point subtract)
        CHECK(absd(out[i] - ref) <= 1e-6 * (1.0 + absd(ref)));
    }
}
