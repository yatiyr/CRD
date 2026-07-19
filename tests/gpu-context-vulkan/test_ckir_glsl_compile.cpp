// test_ckir_glsl.cpp — Phase 3.1.6 v17-b: the CKIR GLSL emitter gate. Emits fused-elementwise compute shaders from
// CKIR subgraphs and PROVES them valid by compiling to SPIR-V through the Vulkan backend's shaderc (ADR-0103) — no GPU
// needed, so the codegen is gated independently of the runtime. Also checks the fusion boundary (non-elementwise ⇒
// rejected). ADR-0098.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hair.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_rt.hpp>

#include <crd/containers/string_view.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
bool compiles(const kir::GlslKernel& k, crd::memory::IAllocator* a)
{
    const auto res = crd::gpu::compile_glsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(k.source), "ckir_ew", a);
    INFO(res.error_message.c_str());
    CHECK(res.ok);
    return res.ok && res.spirv.size() > 0;
}
bool compiles_hlsl(const kir::GlslKernel& k, crd::memory::IAllocator* a)
{
    const auto res = crd::gpu::compile_hlsl_to_spirv(crd::gpu::ShaderStage::Compute, crd::containers::to_view(k.source), "ckir_ew_hlsl", a);
    INFO(res.error_message.c_str());
    CHECK(res.ok);
    return res.ok && res.spirv.size() > 0;
}
} // namespace

// B18-a: the monochrome hair BCSDF (the Chiang R/TT/TRT/TRRT transcendental core — Bessel-I0 longitudinal, trimmed-logistic
// azimuthal, Fresnel/Beer attenuations) lowers to VALID GLSL *and* HLSL. Scalar σₐ ⇒ the whole cone is fusable, so it rides
// the elementwise emitter; proving both backends emit + compile is the portability gate for the BCSDF's op coverage.
TEST_CASE("B18-a: monochrome hair BCSDF emits + compiles on GLSL and HLSL", "[kir][glsl][hair]")
{
    crd::memory::TlsfAllocator alloc(32 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({256});
    namespace hair               = crd::kir::hair;

    const auto in = [&]() { return g.input(sh, kir::DType::F32); };
    const int  sin_to = in();
    const int  phi_o  = in();
    const int  sin_ti = in();
    const int  phi_i  = in();
    const int  h      = in();
    const int  sig    = in(); // scalar absorption ⇒ scalar BCSDF (fusable/elementwise)
    const auto ku     = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const int  cos_to = g.unary(kir::KOp::Sqrt, g.binary(kir::KOp::Max, g.binary(kir::KOp::Sub, ku(1.0), g.binary(kir::KOp::Mul, sin_to, sin_to)), ku(0.0)));
    const int  cos_ti = g.unary(kir::KOp::Sqrt, g.binary(kir::KOp::Max, g.binary(kir::KOp::Sub, ku(1.0), g.binary(kir::KOp::Mul, sin_ti, sin_ti)), ku(0.0)));
    const int  out = hair::hair_bcsdf_eval_angles(g, sin_to, cos_to, phi_o, sin_ti, cos_ti, phi_i, h, ku(1.55), sig, ku(0.3), ku(0.3), ku(2.0));

    kir::GlslKernel kg(&alloc);
    REQUIRE(kir::emit_elementwise_glsl(g, out, &alloc, kg)); // GLSL lowering
    CHECK(compiles(kg, &alloc));
    kir::GlslKernel kh(&alloc);
    REQUIRE(kir::emit_elementwise_hlsl(g, out, &alloc, kh)); // HLSL lowering
    CHECK(compiles_hlsl(kh, &alloc));
}

TEST_CASE("v17-b: fused-elementwise GLSL emitter -> valid SPIR-V", "[kir][glsl]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({256});
    const int                  x  = g.input(sh, kir::DType::F32);
    const int                  y  = g.input(sh, kir::DType::F32);
    // out = (x + y) * exp(x) - abs(y)  — a 4-op elementwise chain that fuses into ONE kernel
    const int s   = g.binary(kir::KOp::Add, x, y);
    const int e   = g.unary(kir::KOp::Exp, x);
    const int p   = g.binary(kir::KOp::Mul, s, e);
    const int ay  = g.unary(kir::KOp::Abs, y);
    const int out = g.binary(kir::KOp::Sub, p, ay);

    kir::GlslKernel k(&alloc);
    REQUIRE(kir::emit_elementwise_glsl(g, out, &alloc, k));
    CHECK(k.n_inputs == 2); // x, y — one global load each; the whole chain fused
    CHECK(compiles(k, &alloc));
}

TEST_CASE("v17-b: emitter covers select / min-max / transcendentals", "[kir][glsl]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh = kir::make_shape({1024});
    const int                  x  = g.input(sh, kir::DType::F32);
    const int                  y  = g.input(sh, kir::DType::F32);
    // out = select(x<y, max(sin(x), tanh(y)), sqrt(abs(x)) / (1 + |y|))
    const int lt   = g.binary(kir::KOp::CmpLt, x, y);
    const int mx   = g.binary(kir::KOp::Max, g.unary(kir::KOp::Sin, x), g.unary(kir::KOp::Tanh, y));
    const int one  = g.constant(1.0, sh, kir::DType::F32);
    const int den  = g.binary(kir::KOp::Add, one, g.unary(kir::KOp::Abs, y));
    const int alt  = g.binary(kir::KOp::Div, g.unary(kir::KOp::Sqrt, g.unary(kir::KOp::Abs, x)), den);
    const int out  = g.select(lt, mx, alt);

    kir::GlslKernel k(&alloc);
    REQUIRE(kir::emit_elementwise_glsl(g, out, &alloc, k));
    CHECK(compiles(k, &alloc));
}

TEST_CASE("v17-b: emitter rejects non-elementwise subtrees (the fusion boundary)", "[kir][glsl]")
{
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const int a = g.input(kir::make_shape({4, 4}), kir::DType::F32);
    const int b = g.input(kir::make_shape({4, 4}), kir::DType::F32);
    const int mm = g.contract(a, b);          // matmul — not elementwise
    const int rd = g.reduce(kir::KOp::ReduceSum, g.binary(kir::KOp::Mul, a, b), 0x3U); // reduce — not elementwise

    kir::GlslKernel k(&alloc);
    CHECK_FALSE(kir::emit_elementwise_glsl(g, mm, &alloc, k));
    CHECK_FALSE(kir::emit_elementwise_glsl(g, rd, &alloc, k));
    // but the pure-elementwise product IS emittable
    const int prod = g.binary(kir::KOp::Mul, a, b);
    CHECK(kir::emit_elementwise_glsl(g, prod, &alloc, k));
}

// B9/RT-1a: the inline ray-query compute kernel (AccelStructDecl + trace_ray_closest) LOWERS to the correct GLSL
// (GL_EXT_ray_query · rayQueryInitializeEXT/ProceedEXT) and HLSL (inline RayQuery<> · TraceRayInline) — the op-coverage gate
// for the RT IR emit. The SPIR-V/DXIL COMPILE + device AS-build + GPU dispatch land together in RT-1b (they share the
// RT-capable shaderc/dxc + Vulkan-RT-device toolchain — a ray_query shader can't be meaningfully compiled without it).
TEST_CASE("B9/RT-1a: inline rayQuery compute kernel lowers to correct GLSL and HLSL", "[kir][glsl][rt]")
{
    const auto has = [](const kir::GlslKernel& k, const char* needle) {
        const char* hay = k.source.c_str();
        for (const char* p = hay; *p != '\0'; ++p) { const char* a = p; const char* b = needle; while (*b != '\0' && *a == *b) { ++a; ++b; } if (*b == '\0') { return true; } }
        return false;
    };
    crd::memory::TlsfAllocator alloc(16 << 20);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh1 = kir::make_shape({1});
    const auto                 cf  = [&](double v) { return g.constant(v, sh1, kir::DType::F32); };
    const auto                 cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh1, kir::DType::U32); };

    const int as   = g.accel_struct_decl(0, 0);
    const int rays = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int out  = g.buffer_decl(kir::DType::F32, 0, 2, true);
    const int mark = g.kernel_stmt_mark();
    const int tid  = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64U)), g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int base = g.binary(kir::KOp::Mul, tid, cu(6U));
    const auto ld  = [&](crd::u32 kk) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(kk))); };
    const int t    = g.trace_ray_closest(as, ld(0), ld(1), ld(2), ld(3), ld(4), ld(5), cf(0.001), cf(1.0e30));
    g.stmt_buffer_store(out, tid, t);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64U;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    kir::GlslKernel kg(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
    INFO("GLSL:\n" << kg.source.c_str());
    CHECK(has(kg, "#version 460"));                 // GL_EXT_ray_query's rayQueryEXT needs 4.60
    CHECK(has(kg, "rayQueryInitializeEXT(rq"));
    CHECK(compiles(kg, &alloc));                    // → SPIR-V (SPV_KHR_ray_query)
    kir::GlslKernel kh(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh));
    INFO("HLSL:\n" << kh.source.c_str());
    CHECK(has(kh, "RayQuery<RAY_FLAG_FORCE_OPAQUE>"));
    CHECK(has(kh, "TraceRayInline(as0"));
    CHECK(compiles_hlsl(kh, &alloc));               // → DXIL/SPIR-V (inline RayQuery, SM 6.5)
}

// D-007 RT-4: the NEE+MIS area-light PATH TRACER — the most demanding RT compute kernel (a runtime `For` sample loop, an unrolled
// bounce chain, BOTH trace kinds — `trace_ray_closest` for shadow rays + `trace_ray_hit` for the continuation — and the MIS
// arithmetic) must lower to VALID GLSL *and* HLSL. This is the both-backend op-coverage gate for the whole path-tracing IR; the
// DX12/DXR device run lands later, but the HLSL must already be well-formed (nullptr emit = a missing op ⇒ the DX12 mirror stalls).
TEST_CASE("D-007 RT-4: NEE+MIS path tracer lowers to valid GLSL and HLSL", "[kir][glsl][rt]")
{
    crd::memory::TlsfAllocator  alloc(32 << 20);
    kir::KGraph                 g(&alloc);
    kir::rt::PathTraceNeeConfig cfg;
    cfg.samples = 8U; // small — this gate is about EMIT validity + compile, not convergence
    cfg.bounces = 2U;
    const kir::KEntry e = kir::rt::build_pathtrace_nee_kernel(g, cfg);

    kir::GlslKernel kg(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg)); // non-null ⇒ every op is wired in the GLSL emitter
    INFO("GLSL:\n" << kg.source.c_str());
    CHECK(compiles(kg, &alloc));                              // → SPIR-V (ray_query + loop + MIS math)
    kir::GlslKernel kh(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh)); // non-null ⇒ every op is wired in the HLSL emitter too
    INFO("HLSL:\n" << kh.source.c_str());
    CHECK(compiles_hlsl(kh, &alloc));                         // → DXIL/SPIR-V (inline RayQuery, SM 6.5) — DX12 mirror is unblocked
}

// D-007 RT-5: the ReSTIR DI RIS-reservoir kernel (streaming WRS over M candidates + a visibility ray, threaded as SSA through the
// unrolled candidate loop) must likewise lower to valid GLSL *and* HLSL — the both-backend gate for the reservoir estimator.
TEST_CASE("D-007 RT-5: ReSTIR DI reservoir kernel lowers to valid GLSL and HLSL", "[kir][glsl][rt]")
{
    crd::memory::TlsfAllocator alloc(32 << 20);
    kir::KGraph                g(&alloc);
    kir::rt::RestirDiConfig    cfg;
    cfg.frames     = 2U;
    cfg.candidates = 8U;
    const kir::KEntry e = kir::rt::build_restir_di_kernel(g, cfg);

    kir::GlslKernel kg(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
    INFO("GLSL:\n" << kg.source.c_str());
    CHECK(compiles(kg, &alloc));
    kir::GlslKernel kh(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh));
    INFO("HLSL:\n" << kh.source.c_str());
    CHECK(compiles_hlsl(kh, &alloc));
}

// D-007 IB-2: the three ReSTIR GI passes (temporal sample+combine, spatial Jacobian reconnection, shade) must lower to valid
// GLSL *and* HLSL — the both-backend gate for the GI-reservoir + reconnection math.
TEST_CASE("D-007 IB-2: ReSTIR GI passes lower to valid GLSL and HLSL", "[kir][glsl][rt]")
{
    crd::memory::TlsfAllocator alloc(48 << 20);
    kir::rt::RestirGiConfig    cfg;
    cfg.width = 16U; cfg.height = 16U; cfg.spatial_k = 4U; cfg.nlights = 1U;
    const char* names[3] = {"gi-temporal", "gi-spatial", "gi-shade"};
    for (int pass = 0; pass < 3; ++pass)
    {
        kir::KGraph g(&alloc);
        const kir::KEntry e = pass == 0 ? kir::rt::build_restir_gi_temporal_kernel(g, cfg)
                            : pass == 1 ? kir::rt::build_restir_gi_spatial_kernel(g, cfg)
                                        : kir::rt::build_restir_gi_shade_kernel(g, cfg);
        kir::GlslKernel kg(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
        INFO("pass=" << names[pass] << " GLSL:\n" << kg.source.c_str());
        CHECK(compiles(kg, &alloc));
        kir::GlslKernel kh(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh));
        INFO("pass=" << names[pass] << " HLSL:\n" << kh.source.c_str());
        CHECK(compiles_hlsl(kh, &alloc));
    }
}

// D-007 IB-1: the FULL production path tracer (many-lights NEE+MIS + emissive hits + Russian roulette + GI) — the most feature-
// dense RT compute kernel — must lower to valid GLSL *and* HLSL.
TEST_CASE("D-007 IB-1: full path tracer lowers to valid GLSL and HLSL", "[kir][glsl][rt]")
{
    crd::memory::TlsfAllocator   alloc(48 << 20);
    kir::KGraph                  g(&alloc);
    kir::rt::PathTraceFullConfig cfg;
    cfg.samples = 8U; cfg.bounces = 3U; cfg.nlights = 2U;
    const kir::KEntry e = kir::rt::build_pathtrace_full_kernel(g, cfg);
    kir::GlslKernel   kg(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
    INFO("GLSL:\n" << kg.source.c_str());
    CHECK(compiles(kg, &alloc));
    kir::GlslKernel kh(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh));
    INFO("HLSL:\n" << kh.source.c_str());
    CHECK(compiles_hlsl(kh, &alloc));
}

// D-007 RT-7: the MANY-LIGHTS NEE kernel (runtime light buffer, ⌊u·N⌋ light selection via Floor+Cast, in-kernel |eu×ev| area)
// must lower to valid GLSL *and* HLSL — the both-backend gate for the light-selection math.
TEST_CASE("D-007 RT-7: many-lights NEE kernel lowers to valid GLSL and HLSL", "[kir][glsl][rt]")
{
    crd::memory::TlsfAllocator alloc(32 << 20);
    kir::KGraph                g(&alloc);
    kir::rt::ManyLightConfig   cfg;
    cfg.samples = 8U; cfg.nlights = 4U;
    const kir::KEntry e = kir::rt::build_manylight_nee_kernel(g, cfg);
    kir::GlslKernel   kg(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
    INFO("GLSL:\n" << kg.source.c_str());
    CHECK(compiles(kg, &alloc));
    kir::GlslKernel kh(&alloc);
    REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh)); // non-null ⇒ Floor/Cast wired in the HLSL emitter
    INFO("HLSL:\n" << kh.source.c_str());
    CHECK(compiles_hlsl(kh, &alloc));
}

// D-007 RT-5b/c: the three SPATIOTEMPORAL ReSTIR passes (temporal-combine, spatial-neighbour resample with integer pixel
// coords + trig-disk gather, shade) must all lower to valid GLSL *and* HLSL — this is the both-backend gate for the pixel-grid
// math (u32 Mod/Div for x,y coords, f32↔u32 Cast for the neighbour index) that only these kernels exercise.
TEST_CASE("D-007 RT-5: spatiotemporal ReSTIR passes lower to valid GLSL and HLSL", "[kir][glsl][rt]")
{
    crd::memory::TlsfAllocator alloc(48 << 20);
    kir::rt::RestirStConfig    cfg;
    cfg.width = 16U; cfg.height = 16U; cfg.m_initial = 2U; cfg.spatial_k = 4U;
    const char* names[3] = {"temporal", "spatial", "shade"};
    for (int pass = 0; pass < 3; ++pass)
    {
        kir::KGraph g(&alloc);
        const kir::KEntry e = pass == 0 ? kir::rt::build_restir_temporal_kernel(g, cfg)
                            : pass == 1 ? kir::rt::build_restir_spatial_kernel(g, cfg)
                                        : kir::rt::build_restir_shade_kernel(g, cfg);
        kir::GlslKernel kg(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kg));
        INFO("pass=" << names[pass] << " GLSL:\n" << kg.source.c_str());
        CHECK(compiles(kg, &alloc));
        kir::GlslKernel kh(&alloc);
        REQUIRE(kir::emit_compute_kernel_hlsl(g, e, &alloc, kh)); // non-null ⇒ Mod/Div/Cast all wired in the HLSL emitter
        INFO("pass=" << names[pass] << " HLSL:\n" << kh.source.c_str());
        CHECK(compiles_hlsl(kh, &alloc));
    }
}
