// test_vulkan_context.cpp — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own,
// with a compute queue + (on capable adapters) the coopmat2 tensor feature — no rendering RHI, no swapchain. This is
// the foundation kir-vulkan migrates onto in v17-i-b.

#include <crd/gpu/vulkan_compute_context.hpp> // B-cmp: create_pipeline_from_spirv + the portable dispatch surface
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/kir/ckir.hpp>      // C1-c: create_program(KGraph, KEntry) — the IR on-ramp
#include <crd/kir/ckir_fft.hpp>    // B-cmp Phase 1: build_fft1d_radix2 (the CKIR FFT authoring layer)
#include <crd/kir/ckir_reduce.hpp> // B-cmp: build_reduce (the CKIR device-wide reduction)
#include <crd/kir/ckir_scan.hpp>   // B-cmp: build_scan (the CKIR device-wide prefix sum)
#include <crd/kir/ckir_sort.hpp>   // B-cmp: build_sort_* (the CKIR stable LSD radix sort)
#include <crd/kir/ckir_glsl.hpp> // B-cmp: emit_compute_kernel_glsl (the shared-memory compute-kernel emitter)
#include <crd/kir/ckir_hlsl.hpp> // B3-d: emit_stage_hlsl (the HLSL VS/FS emitter)

#include <crd/math/cmath.hpp> // Phase-1 FFT: host-side twiddle table (cos/sin)

#include <ckir_kernel_dispatch.hpp> // B-cmp: the SHARED both-backend kernel dispatch + oracle-compare harness
#include <ckir_raster_triangle.hpp> // B3-e: the SHARED, backend-neutral CKIR triangle (identical on Vulkan + DX12)

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib> // std::abs(int) for the readback tolerance
#include <cstring>
#include <memory>

namespace gpu = crd::gpu;

TEST_CASE("v17-i-a: headless Vulkan compute context via the GpuContextManager", "[gpu-context][vulkan][gpu]")
{
    gpu::GpuContextManager mgr;
    gpu::GpuContextConfig  cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;

    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }

    gpu::IGpuContext* held = mgr.add(std::move(ctx)); // manager takes ownership
    REQUIRE(held != nullptr);
    CHECK(held->valid());
    CHECK(held->backend() == gpu::GpuBackend::Vulkan);
    CHECK(mgr.get(gpu::GpuBackend::Vulkan) == held); // the manager serves it back
    CHECK(mgr.count() == 1);

    auto* vk = static_cast<gpu::VulkanGpuContext*>(held); // backend()==Vulkan ⇒ safe downcast
    CHECK(vk->vk_instance() != VK_NULL_HANDLE);
    CHECK(vk->vk_device() != VK_NULL_HANDLE);
    CHECK(vk->compute_queue() != VK_NULL_HANDLE); // a real compute queue, not borrowed from a graphics-only path
    std::printf("[gpu-context-vulkan] adapter=%s  coopmat2=%s  compute_family=%u\n",
                vk->adapter_name(), vk->cooperative_matrix2() ? "YES" : "no", vk->compute_family());
    // coopmat2 is the tensor lever (present on the RTX 4070 Ti Super); a soft note so the test stays portable.
    if (!vk->cooperative_matrix2()) { WARN("adapter has no VK_NV_cooperative_matrix2 — tensor tier will be unavailable"); }
}

TEST_CASE("D-008 C0: the program seam -- cooked SPIR-V -> IGpuProgram (ADR-0103)", "[gpu-context][vulkan][gpu][program]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;

    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }

    crd::memory::TlsfAllocator alloc(1U << 20U);

    // Compile a trivial compute kernel to SPIR-V through the Vulkan backend's OWN compiler (relocated from crd-shader),
    // then mint a program from the cooked bytes. End-to-end proof the seam works: language + bytecode stay inside the
    // backend; the caller only ever holds the opaque IGpuProgram.
    static constexpr const char* kSrc = R"glsl(
#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) buffer B { float v[]; };
void main() { v[0] = 1.0; }
)glsl";

    const auto spv = gpu::compile_glsl_to_spirv(
        gpu::ShaderStage::Compute, crd::containers::StringView(kSrc, std::strlen(kSrc)), "seam_probe", &alloc);
    REQUIRE(spv.ok);                 // the relocated shaderc compiler works
    REQUIRE(spv.spirv.size() >= 4U); // a real SPIR-V module (magic word + body)

    auto program = ctx->create_program(
        gpu::ShaderStage::Compute, crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()));
    REQUIRE(program != nullptr);
    CHECK(program->valid());
    CHECK(program->stage() == gpu::ShaderStage::Compute);

    // Malformed bytecode (not a whole number of 32-bit words) is rejected, not crashed on.
    const crd::u8 junk[3] = {1U, 2U, 3U};
    auto bad = ctx->create_program(gpu::ShaderStage::Compute, crd::containers::ConstSpan<crd::u8>(junk, 3));
    CHECK(bad == nullptr);
}

TEST_CASE("D-008 C1-c: create_program(KGraph, KEntry) -- the IR on-ramp (ADR-0103)",
          "[gpu-context][vulkan][gpu][program]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }

    // A small elementwise COMPUTE graph: out = (x + y) * exp(x). The IR is the currency — hand it straight to the seam.
    crd::memory::TlsfAllocator alloc(4U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Shape           sh  = kir::make_shape({256});
    const int                  x   = g.input(sh, kir::DType::F32);
    const int                  y   = g.input(sh, kir::DType::F32);
    const int                  out = g.binary(kir::KOp::Mul, g.binary(kir::KOp::Add, x, y), g.unary(kir::KOp::Exp, x));

    kir::KEntry e;
    e.stage       = kir::KStage::Compute;
    e.n_out       = 1;
    e.out[0].node = out; // a compute entry names its output node

    auto prog = ctx->create_program(g, e);
    REQUIRE(prog != nullptr); // graph → GLSL (crd-kir emitter) → SPIR-V (our compiler) → program
    CHECK(prog->valid());
    CHECK(prog->stage() == gpu::ShaderStage::Compute);
    // The emitted GLSL actually compiled to real SPIR-V — proof the currency reached the seam, not just a stub.
    auto* vprog = static_cast<gpu::VulkanGpuProgram*>(prog.get());
    CHECK(vprog->vk_spirv().size() >= 4U);

    // A vertex entry over this COMPUTE graph is refused loudly: it reads a storage-buffer `Input` (invalid in raster) and
    // names no clip position. B3-c's real raster path is the next test (a well-formed VS/FS graph).
    kir::KEntry vs;
    vs.stage       = kir::KStage::Vertex;
    vs.n_out       = 1;
    vs.out[0].node = out;
    CHECK(ctx->create_program(g, vs) == nullptr);
}

TEST_CASE("D-007 B3-c: create_program(KGraph, KEntry) emits VERTEX + FRAGMENT programs through the seam",
          "[gpu-context][vulkan][gpu][program][raster]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // VERTEX: attribute(vec4, loc 0) -> gl_Position; a second attribute(vec4, loc 1) passed through as interpolant(loc 0).
    kir::KGraph vg(&alloc);
    const int   a_pos = vg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    const int   a_col = vg.stage_in(kir::KType::vec(kir::DType::F32, 4), 1);
    kir::KEntry ve;
    ve.stage    = kir::KStage::Vertex;
    ve.position = a_pos; // a vertex entry MUST write clip position
    ve.n_out    = 1;
    ve.out[0]   = {a_col, 0};
    auto vprog  = ctx->create_program(vg, ve);
    REQUIRE(vprog != nullptr); // KIR vertex entry -> GLSL VS (emit_stage_glsl) -> SPIR-V -> program, all behind the seam
    CHECK(vprog->stage() == gpu::ShaderStage::Vertex);
    CHECK(static_cast<gpu::VulkanGpuProgram*>(vprog.get())->vk_spirv().size() >= 4U);

    // FRAGMENT: interpolant(vec4, loc 0) -> colour attachment(loc 0).
    kir::KGraph fg(&alloc);
    const int   f_in = fg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    kir::KEntry fe;
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {f_in, 0};
    auto fprog = ctx->create_program(fg, fe);
    REQUIRE(fprog != nullptr); // KIR fragment entry -> GLSL FS -> SPIR-V -> program
    CHECK(fprog->stage() == gpu::ShaderStage::Fragment);
    CHECK(static_cast<gpu::VulkanGpuProgram*>(fprog.get())->vk_spirv().size() >= 4U);

    // The gate BITES: a vertex entry that names no clip position is refused loudly (not guessed).
    kir::KGraph bg(&alloc);
    const int   b_pos = bg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    kir::KEntry be;
    be.stage  = kir::KStage::Vertex; // position left at -1
    be.n_out  = 1;
    be.out[0] = {b_pos, 0};
    CHECK(ctx->create_program(bg, be) == nullptr);
}

TEST_CASE("D-007 B3-d: emit_stage_hlsl VERTEX + FRAGMENT HLSL compiles to SPIR-V via DXC (the DX12 mirror)",
          "[gpu-context][vulkan][gpu][program][raster][hlsl]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // Probe: is dxc available on this host? (compile_hlsl_to_spirv returns ok=false + a "dxcompiler not loaded" message
    // when the DLL is missing — Linux configs without dxc, etc.) Soft-skip if absent, like the geometry HLSL test.
    const auto probe = crd::gpu::compile_hlsl_to_spirv(
        crd::gpu::ShaderStage::Vertex,
        crd::containers::StringView("float4 main() : SV_Position { return float4(0,0,0,1); }"), "b3d_probe", &alloc);
    if (!probe.ok)
    {
        WARN("dxc unavailable; skipping B3-d HLSL raster gate");
        return;
    }

    // VERTEX: attribute(vec4, loc 0) -> gl_Position; attribute(vec4, loc 1) -> interpolant(loc 0).
    kir::KGraph vg(&alloc);
    const int   a_pos = vg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    const int   a_col = vg.stage_in(kir::KType::vec(kir::DType::F32, 4), 1);
    kir::KEntry ve;
    ve.stage    = kir::KStage::Vertex;
    ve.position = a_pos;
    ve.n_out    = 1;
    ve.out[0]   = {a_col, 0};
    kir::GlslKernel vk(&alloc);
    REQUIRE(kir::emit_stage_hlsl(vg, ve, &alloc, vk)); // KIR vertex entry -> HLSL VS text
    const auto vspv = crd::gpu::compile_hlsl_to_spirv(
        crd::gpu::ShaderStage::Vertex, crd::containers::to_view(vk.source), "b3d_vs", &alloc);
    INFO(crd::containers::String(vspv.error_message).c_str());
    REQUIRE(vspv.ok); // the emitted HLSL VS is valid -> dxc lowers it to SPIR-V
    CHECK(vspv.spirv.size() >= 4U);

    // FRAGMENT: interpolant(vec4, loc 0) -> colour attachment(loc 0).
    kir::KGraph fg(&alloc);
    const int   f_in = fg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    kir::KEntry fe;
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {f_in, 0};
    kir::GlslKernel fk(&alloc);
    REQUIRE(kir::emit_stage_hlsl(fg, fe, &alloc, fk));
    const auto fspv = crd::gpu::compile_hlsl_to_spirv(
        crd::gpu::ShaderStage::Fragment, crd::containers::to_view(fk.source), "b3d_fs", &alloc);
    INFO(crd::containers::String(fspv.error_message).c_str());
    REQUIRE(fspv.ok);
    CHECK(fspv.spirv.size() >= 4U);

    // The gate BITES: a vertex entry that names no clip position is refused loudly.
    kir::KGraph bg(&alloc);
    const int   b_pos = bg.stage_in(kir::KType::vec(kir::DType::F32, 4), 0);
    kir::KEntry be;
    be.stage  = kir::KStage::Vertex; // no position
    be.n_out  = 1;
    be.out[0] = {b_pos, 0};
    kir::GlslKernel bk(&alloc);
    CHECK_FALSE(kir::emit_stage_hlsl(bg, be, &alloc, bk));
}

TEST_CASE("D-008 C2-a: a windowed context is render-capable; headless is unchanged", "[gpu-context][vulkan][gpu]")
{
    // Headless (the compute default) stays render-INCAPABLE — the compute path is byte-for-byte unchanged.
    gpu::GpuContextConfig headless;
    headless.backend  = gpu::GpuBackend::Vulkan;
    headless.headless = true;
    auto hctx         = gpu::create_vulkan_gpu_context(headless);
    if (hctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    CHECK_FALSE(static_cast<gpu::VulkanGpuContext*>(hctx.get())->render_capable());

    // A WINDOWED context additionally enables surface + swapchain ⇒ render_capable (the C2 device can present).
    gpu::GpuContextConfig windowed;
    windowed.backend  = gpu::GpuBackend::Vulkan;
    windowed.headless = false;
    auto wctx         = gpu::create_vulkan_gpu_context(windowed);
    REQUIRE(wctx != nullptr);
    auto* wvk = static_cast<gpu::VulkanGpuContext*>(wctx.get());
    CHECK(wvk->render_capable());     // surface + VK_KHR_swapchain + a graphics queue
    CHECK(wvk->graphics_capable());
    CHECK(wvk->graphics_queue() != VK_NULL_HANDLE);
    CHECK(wvk->compute_queue() != VK_NULL_HANDLE); // still async-compute-capable — ONE device, both concerns
}

TEST_CASE("D-008 C1-a: graphics-capable context + IRasterContext clear/readback (ADR-0103)",
          "[gpu-context][vulkan][gpu][raster]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;

    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());

    // The converged device is graphics-capable AND still async-compute-capable (ADR-0099 "one device, both concerns").
    REQUIRE(vk->graphics_capable());
    CHECK(vk->graphics_queue() != VK_NULL_HANDLE);
    CHECK(vk->compute_queue() != VK_NULL_HANDLE);
    std::printf("[gpu-context-vulkan] graphics_family=%u  shader_object=%s\n", vk->graphics_family(),
                vk->shader_object() ? "YES" : "no");

    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());

    auto target = raster->create_color_target(8U, 8U);
    REQUIRE(target != nullptr);
    CHECK(target->width() == 8U);
    CHECK(target->height() == 8U);

    // Clear to (0.25, 0.5, 0.75, 1.0) → RGBA8 ~ (64, 128, 191, 255) and read it back off the GPU.
    raster->clear(*target, gpu::ClearColor{0.25F, 0.5F, 0.75F, 1.0F});
    const crd::u32 px = target->read_pixel(3U, 5U);
    const auto     r  = static_cast<int>(px & 0xFFU);
    const auto     g  = static_cast<int>((px >> 8U) & 0xFFU);
    const auto     b  = static_cast<int>((px >> 16U) & 0xFFU);
    const auto     a  = static_cast<int>((px >> 24U) & 0xFFU);
    CHECK(std::abs(r - 64) <= 2);  // unorm rounding tolerance
    CHECK(std::abs(g - 128) <= 2);
    CHECK(std::abs(b - 191) <= 2);
    CHECK(a == 255);
}

TEST_CASE("D-008 C1-b: shader-object DRAW -- a red triangle over a blue clear (ADR-0103)",
          "[gpu-context][vulkan][gpu][raster]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;

    auto ctx = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping the draw");
        return;
    }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // Attributeless triangle: the VS emits 3 clip-space positions from gl_VertexIndex (a big triangle covering the
    // centre but not the corners); the FS paints it red. Compiled to SPIR-V through the Vulkan backend's own compiler.
    static constexpr const char* kVs = R"glsl(
#version 450
void main() {
    vec2 p[3] = vec2[](vec2(0.0, -0.8), vec2(0.8, 0.8), vec2(-0.8, 0.8));
    gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
)glsl";
    static constexpr const char* kFs = R"glsl(
#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(1.0, 0.0, 0.0, 1.0); }
)glsl";

    const auto vs_spv = gpu::compile_glsl_to_spirv(
        gpu::ShaderStage::Vertex, crd::containers::StringView(kVs, std::strlen(kVs)), "tri_vs", &alloc);
    const auto fs_spv = gpu::compile_glsl_to_spirv(
        gpu::ShaderStage::Fragment, crd::containers::StringView(kFs, std::strlen(kFs)), "tri_fs", &alloc);
    REQUIRE(vs_spv.ok);
    REQUIRE(fs_spv.ok);

    auto vs_prog = ctx->create_program(gpu::ShaderStage::Vertex,
                                       crd::containers::ConstSpan<crd::u8>(vs_spv.spirv.data(), vs_spv.spirv.size()));
    auto fs_prog = ctx->create_program(gpu::ShaderStage::Fragment,
                                       crd::containers::ConstSpan<crd::u8>(fs_spv.spirv.data(), fs_spv.spirv.size()));
    REQUIRE(vs_prog != nullptr);
    REQUIRE(fs_prog != nullptr);

    auto program = raster->create_raster_program(*vs_prog, *fs_prog);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U); // blue clear, draw the triangle

    // Centre is inside the triangle → RED; a corner is outside → BLUE clear. This is the whole seam end-to-end: IR-free
    // trivial shaders → the Vulkan backend's compiler → IGpuProgram → shader objects → dynamic-rendering draw → readback.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low  ⇒ red
    CHECK((corner & 0xFFU) <= 5U);            // R low
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high ⇒ blue clear
}

TEST_CASE("D-007 B3-e: IR-authored triangle draws on Vulkan (CKIR graph -> SPIR-V -> shader objects -> pixels)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping the draw");
        return;
    }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // The SAME shared CKIR triangle the DX12 B3-e test draws — one IR, both backends.
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    auto vs = ctx->create_program(vg, ve); // KIR -> GLSL -> SPIR-V, all behind the seam
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  ⇒ red (inside the IR-authored triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   ⇒ blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high
}

TEST_CASE("D-007 B1-a: IR fragment derivatives (dFdx/dFdy of FragCoord.x) draw on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping the draw");
        return;
    }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_derivative_fs(fg, fe); // colour = (dFdx(FragCoord.x), dFdy(FragCoord.x), 0, 1)

    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // dFdx(FragCoord.x) == 1 (screen x rises 1/pixel) ⇒ R≈255; dFdy(FragCoord.x) == 0 ⇒ G≈0.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    CHECK((centre & 0xFFU) >= 250U);        // R = dFdx == 1.0
    CHECK(((centre >> 8U) & 0xFFU) <= 5U);  // G = dFdy == 0.0
}

TEST_CASE("D-007 B1-b: IR fragment discard (alpha-test on FragCoord.x) draws on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping the draw");
        return;
    }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_discard_fs(fg, fe); // red, but discards where FragCoord.x < 16 (left half → clear)

    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // Both (12,16) and (20,16) are inside the triangle. Right of x=16 ⇒ kept (red); left ⇒ discarded (blue clear shows).
    const crd::u32 kept = target->read_pixel(20U, 16U);
    const crd::u32 cut  = target->read_pixel(12U, 16U);
    CHECK((kept & 0xFFU) >= 250U);          // R high  ⇒ red survived
    CHECK((cut & 0xFFU) <= 5U);             // R low
    CHECK(((cut >> 16U) & 0xFFU) >= 250U);  // B high  ⇒ discarded, blue clear shows through
}

TEST_CASE("D-007 B1-c: IR flat integer interpolant (VS->FS) draws on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object())
    {
        WARN("adapter has no VK_EXT_shader_object; skipping the draw");
        return;
    }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_flat_vs(vg, ve); // flat int payload = 200 at location 0
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_flat_fs(fg, fe); // reads the flat int, colour = (200/255, 0, 0, 1)

    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // an int varying only compiles because `flat` was emitted on both sides

    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // The flat int (200) reached the fragment intact ⇒ R ≈ 200/255 ⇒ unorm8 200.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 red    = centre & 0xFFU;
    CHECK(red >= 196U);
    CHECK(red <= 204U);
    CHECK(((centre >> 16U) & 0xFFU) <= 5U); // B low (not the clear)
}

TEST_CASE("D-007 B1-c: IR noperspective vs smooth interpolant diverge on a perspective triangle (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("adapter has no VK_EXT_shader_object; skipping the draw"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_noperspective_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_noperspective_fs(fg, fe);

    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    // R = perspective-correct (smooth ≈ 0.069 → ~18), G = screen-linear (noperspective ≈ 0.225 → ~57). If `noperspective`
    // were dropped both would interpolate perspective-correct and R == G — so a clear gap is the biting gate.
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const auto     r      = static_cast<int>(centre & 0xFFU);
    const auto     g      = static_cast<int>((centre >> 8U) & 0xFFU);
    CHECK(g > r + 12); // screen-linear noticeably exceeds perspective-correct at the centre
    CHECK(r < 70);     // neither channel saturated (sanity that both interpolants actually landed)
    CHECK(g < 110);
}

TEST_CASE("D-007 B1-c: IR centroid interpolation samples inside coverage on an MSAA target (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("adapter has no VK_EXT_shader_object; skipping the draw"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_centroid_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_centroid_fs(fg, fe);

    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target_ms(dim, dim, 4U);
    REQUIRE(target != nullptr); // a null target ⇒ 4x MSAA unsupported for RGBA8 (would be a hard fail on this adapter)
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    // R = centre-sampled (smooth), G = centroid-sampled. Equal (bit-identical) on fully-covered interior pixels; they
    // diverge at partially-covered EDGE pixels (centroid stays inside coverage, centre may extrapolate). If `centroid`
    // were dropped both interpolate identically ⇒ R == G on EVERY pixel ⇒ zero differing pixels. So a band of differing
    // edge pixels is the biting gate.
    int max_diff = 0;
    int n_diff   = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 px = target->read_pixel(x, y);
            const auto     r  = static_cast<int>(px & 0xFFU);
            const auto     g  = static_cast<int>((px >> 8U) & 0xFFU);
            const int      d  = r > g ? r - g : g - r;
            if (d > max_diff) { max_diff = d; }
            if (d >= 2) { ++n_diff; } // >=2 ignores any ±1 unorm rounding asymmetry between the two resolves
        }
    }
    WARN("[centroid vulkan] max|R-G| = " << max_diff << "  n_diff(>=2) = " << n_diff);
    CHECK(n_diff >= 6); // a band of edge pixels where centroid pulled the sample inside coverage
}

TEST_CASE("D-007 B1-c: IR sample interpolation forces per-sample shading on an MSAA target (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("adapter has no VK_EXT_shader_object; skipping the draw"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    constexpr crd::u32 dim = 32U;

    // Draw the ramp+step with a given interpolation qualifier and count "intermediate" (antialiased) resolved pixels.
    const auto count_intermediate = [&](kir::Interp interp) -> int
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_ramp_vs(vg, ve, interp);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        crd::gputest::build_step_fs(fg, fe, interp);
        auto vs = ctx->create_program(vg, ve);
        auto fs = ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target_ms(dim, dim, 4U);
        REQUIRE(target != nullptr);
        raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);
        int n = 0;
        for (crd::u32 y = 0; y < dim; ++y)
        {
            for (crd::u32 x = 0; x < dim; ++x)
            {
                const auto rr = static_cast<int>(target->read_pixel(x, y) & 0xFFU);
                if (rr >= 40 && rr <= 215) { ++n; } // a partial (antialiased) coverage of the step
            }
        }
        return n;
    };

    const int n_sample = count_intermediate(kir::Interp::Sample);
    const int n_smooth = count_intermediate(kir::Interp::Smooth);
    WARN("[sample vulkan] n_sample=" << n_sample << " n_smooth=" << n_smooth);
    CHECK(n_smooth == 0);    // per-PIXEL shading of a step over a full-screen tri ⇒ every pixel a hard 0/255
    CHECK(n_sample >= 4);    // per-SAMPLE shading antialiases the threshold column ⇒ intermediate greys appear
}

// B1-d shared Vulkan setup: a graphics-capable headless context + raster context, or a skip (vk == nullptr).
namespace
{
struct VkRaster
{
    std::unique_ptr<gpu::IGpuContext>    ctx;
    gpu::VulkanGpuContext*               vk = nullptr;
    std::unique_ptr<gpu::IRasterContext> raster;
};
inline VkRaster vk_raster_or_skip()
{
    VkRaster              r;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    r.ctx        = gpu::create_vulkan_gpu_context(cfg);
    if (r.ctx == nullptr) { return r; }
    r.vk = static_cast<gpu::VulkanGpuContext*>(r.ctx.get());
    if (!r.vk->shader_object()) { r.vk = nullptr; return r; }
    r.raster = gpu::create_vulkan_raster_context(*r.vk);
    return r;
}

// B1-e: count horizontal even-x neighbour pairs (2i, 2i+1) whose R channel is EQUAL. A coarse VRS rate makes each 2×2
// block share one fragment invocation ⇒ those pairs become equal; at 1×1 the ramp FS leaves them distinct.
inline int count_equal_even_pairs(gpu::IRasterTarget& t, crd::u32 dim)
{
    int n = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 i = 0; i + 1U < dim; i += 2U)
        {
            const int rl = static_cast<int>(t.read_pixel(i, y) & 0xFFU);
            const int rr = static_cast<int>(t.read_pixel(i + 1U, y) & 0xFFU);
            if (rl == rr) { ++n; }
        }
    }
    return n;
}
} // namespace

TEST_CASE("D-007 B1-d: IR frag-depth write drives the depth test (Vulkan)", "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_fragdepth_fs(fg, fe); // red + gl_FragDepth = FragCoord.x/32

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    // Clear depth to 0.5, LessEqual: primitive depth is 0 (would pass everywhere) — so any FAIL is the WRITTEN depth.
    r.raster->draw_depth(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.5F, gpu::DepthCompare::LessEqual,
                         3U);

    const crd::u32 left  = target->read_pixel(4U, dim / 2U);  // depth ≈ 0.14 ≤ 0.5 ⇒ passes ⇒ red
    const crd::u32 right = target->read_pixel(28U, dim / 2U); // depth ≈ 0.89 > 0.5 ⇒ fails ⇒ blue clear
    CHECK((left & 0xFFU) > 200U);           // R high on the left
    CHECK(((left >> 16U) & 0xFFU) < 60U);   // B low on the left
    CHECK((right & 0xFFU) < 60U);           // R low on the right
    CHECK(((right >> 16U) & 0xFFU) > 200U); // B high (the clear) on the right
}

TEST_CASE("D-007 B1-d: IR conservative depth (DepthGreater) frag-depth write (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_fragdepth_fs(fg, fe);
    fe.depth_mode = kir::DepthMode::Greater; // the ramp only raises depth above the primitive's 0 ⇒ promise holds

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr); // emits layout(depth_greater) out float gl_FragDepth; ⇒ must compile to valid SPIR-V
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_depth(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.5F, gpu::DepthCompare::LessEqual,
                         3U);
    // Conservative depth is a hint for early-Z; the depth test still uses the written value ⇒ same split as the plain case.
    CHECK((target->read_pixel(4U, dim / 2U) & 0xFFU) > 200U);            // left red
    CHECK(((target->read_pixel(28U, dim / 2U) >> 16U) & 0xFFU) > 200U);  // right blue
}

TEST_CASE("D-007 B1-d: IR early_fragment_tests forces early-Z (Vulkan)", "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_early_fragment_fs(fg, fe); // red, early_fragment_tests = true

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // emits layout(early_fragment_tests) in; ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_depth_target(dim, dim);
    REQUIRE(target != nullptr);
    // Primitive depth 0 ≤ 0.5 clear (LessEqual) ⇒ the early test passes everywhere ⇒ the whole target is red.
    r.raster->draw_depth(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 0.5F, gpu::DepthCompare::LessEqual,
                         3U);
    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    CHECK((centre & 0xFFU) > 200U);         // red (the FS ran and passed the early depth test)
    CHECK(((centre >> 16U) & 0xFFU) < 60U); // not the blue clear
}

TEST_CASE("D-007 B1-e: per-draw VRS 2x2 coarsens shading (Vulkan)", "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    if (!r.vk->fragment_shading_rate()) { WARN("adapter has no VK_KHR_fragment_shading_rate; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe); // R = FragCoord.x/32 — a per-pixel ramp

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim = 32U;
    auto               t1  = r.raster->create_color_target(dim, dim);
    auto               t2  = r.raster->create_color_target(dim, dim);
    REQUIRE(t1 != nullptr);
    REQUIRE(t2 != nullptr);
    r.raster->draw(*t1, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U); // 1x1 baseline
    r.raster->draw_vrs(*t2, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::ShadingRate::Rate2x2,
                       gpu::ShadingRateCombiner::Keep, 3U); // per-draw 2x2

    const int n1 = count_equal_even_pairs(*t1, dim);
    const int n2 = count_equal_even_pairs(*t2, dim);
    WARN("[vrs per-draw vulkan] n_1x1=" << n1 << " n_2x2=" << n2);
    CHECK(n2 > n1 + 100); // 2x2 makes each block's even-x neighbours equal; 1x1 leaves them distinct
    CHECK(n1 < 80);       // sanity: the 1x1 ramp actually varies per pixel
}

TEST_CASE("D-007 B1-e: per-primitive VRS out (gl_PrimitiveShadingRateEXT) coarsens shading (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    if (!r.vk->fragment_shading_rate()) { WARN("adapter has no VK_KHR_fragment_shading_rate; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vrs_primitive_vs(vg, ve); // VS outputs gl_PrimitiveShadingRateEXT = 2x2
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr); // the VS emits gl_PrimitiveShadingRateEXT ⇒ must compile to valid SPIR-V
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    // pipeline rate 1x1, but the PRIMITIVE rate (2x2) REPLACES it ⇒ the shader-output rate drives the coarsening.
    r.raster->draw_vrs(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::ShadingRate::Rate1x1,
                       gpu::ShadingRateCombiner::Replace, 3U);

    const int n = count_equal_even_pairs(*target, dim);
    WARN("[vrs per-primitive vulkan] n_equal=" << n);
    CHECK(n > static_cast<int>(dim * dim / 4U)); // the primitive-output 2x2 rate made blocks uniform
}

TEST_CASE("D-007 B1-e: attachment (image) VRS 2x2 coarsens shading (Vulkan)", "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    if (!r.vk->fragment_shading_rate()) { WARN("adapter has no VK_KHR_fragment_shading_rate; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_vrs_target(dim, dim, gpu::ShadingRate::Rate2x2);
    REQUIRE(target != nullptr);
    // pipeline 1x1, no primitive rate; the per-tile ATTACHMENT rate (2x2) REPLACES ⇒ coarse blocks.
    r.raster->draw_vrs(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, gpu::ShadingRate::Rate1x1,
                       gpu::ShadingRateCombiner::Keep, 3U);

    const int n = count_equal_even_pairs(*target, dim);
    WARN("[vrs attachment vulkan] n_equal=" << n);
    CHECK(n > static_cast<int>(dim * dim / 4U)); // the attachment 2x2 rate made blocks uniform
}

// B1-f: count target pixels whose R channel is high (the constant-red triangle's coverage), over a non-red clear.
namespace
{
inline int count_red(gpu::IRasterTarget& t, crd::u32 dim)
{
    int n = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x) { if ((t.read_pixel(x, y) & 0xFFU) > 200U) { ++n; } }
    }
    return n;
}
} // namespace

TEST_CASE("D-007 B1-f: conservative OVERESTIMATE raster covers more pixels (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    if (!r.raster->supports_conservative_raster()) { WARN("adapter has no conservative raster; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_small_triangle_vs(vg, ve); // a small TILTED triangle (many partially-covered edge pixels)
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // constant red

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32    dim    = 64U;
    auto                  t_norm = r.raster->create_color_target(dim, dim);
    auto                  t_over = r.raster->create_color_target(dim, dim);
    const gpu::ClearColor blue{0.0F, 0.0F, 1.0F, 1.0F};
    REQUIRE(t_norm != nullptr);
    REQUIRE(t_over != nullptr);
    r.raster->draw_conservative(*t_norm, *program, blue, gpu::ConservativeMode::Off, 3U);          // normal raster
    r.raster->draw_conservative(*t_over, *program, blue, gpu::ConservativeMode::Overestimate, 3U); // + the edge rim

    const int n_norm = count_red(*t_norm, dim);
    const int n_over = count_red(*t_over, dim);
    WARN("[conservative vulkan] n_normal=" << n_norm << " n_over=" << n_over);
    CHECK(n_norm > 0);      // the triangle has a solid interior
    CHECK(n_over > n_norm); // overestimate additionally covers the partially-touched edge pixels
}

TEST_CASE("D-007 B1-f: inner coverage distinguishes fully-covered from edge pixels (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    if (!r.raster->supports_inner_coverage()) { WARN("adapter has no inner coverage; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_small_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_inner_coverage_fs(fg, fe); // white where fully covered, black at the edge

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // emits gl_FragFullyCoveredNV ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    // Overestimate generates the edge fragments; inner coverage is 0 there (black) and 1 in the interior (white). Blue clear.
    r.raster->draw_conservative(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F},
                                gpu::ConservativeMode::Overestimate, 3U);

    int white = 0; // interior, inner coverage 1
    int black = 0; // edge rim, inner coverage 0 (distinct from the blue background)
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 px = target->read_pixel(x, y);
            const crd::u32 rr = px & 0xFFU;
            const crd::u32 gg = (px >> 8U) & 0xFFU;
            const crd::u32 bb = (px >> 16U) & 0xFFU;
            if (rr > 200U && gg > 200U && bb > 200U) { ++white; }
            else if (rr < 50U && gg < 50U && bb < 50U) { ++black; }
        }
    }
    WARN("[inner coverage vulkan] white=" << white << " black=" << black);
    CHECK(white > 0); // the interior is fully covered
    CHECK(black > 0); // an edge rim is only partially covered ⇒ inner coverage VARIES across the primitive
}

TEST_CASE("D-007 B1-f: fragment interlock RMW counter is deterministic (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    if (!r.raster->supports_fragment_interlock()) { WARN("adapter has no fragment interlock; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_interlock_vs(vg, ve); // the base triangle authored to draw TWICE from a 6-vertex call
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_interlock_fs(fg, fe, dim); // storage[y*dim + x] += 1 under rasterizer-ordered access

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // emits layout(pixel_interlock_ordered) in; + begin/endInvocationInterlockARB ⇒ valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    auto target  = r.raster->create_color_target(dim, dim);
    auto storage = r.raster->create_storage_buffer(dim * dim * 4U);
    REQUIRE(target != nullptr);
    REQUIRE(storage != nullptr);
    r.raster->draw_storage(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *storage, 6U); // two triangles

    // The centre pixel is covered by BOTH primitives; interlock serialises the two RMWs ⇒ EXACTLY 2. Corner = background = 0.
    const crd::u32 c_centre = storage->read_u32((dim / 2U) * dim + dim / 2U);
    const crd::u32 c_corner = storage->read_u32(0U);
    WARN("[interlock vulkan] centre=" << c_centre << " corner=" << c_corner);
    CHECK(c_centre == 2U);
    CHECK(c_corner == 0U);
    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) > 200U); // the colour target still shows coverage (red)
}

TEST_CASE("D-007 B2-a: IR 2D texture sample (left-red/right-green) draws on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_sample_fs(fg, fe); // sample tex_0_1 through samp_0_2 at the UV interpolant

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // the FS declares a separable texture2D + sampler and samples them ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    crd::u8            tex_data[tw * tw * 4U];
    crd::gputest::fill_left_red_right_green(tex_data, tw, tw);
    auto texture = r.raster->create_texture(tw, tw, tex_data);
    REQUIRE(texture != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_textured(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *texture, 3U);

    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);      // UV.x ≈ 0.25 ⇒ left texels ⇒ red
    const crd::u32 right = target->read_pixel(3U * dim / 4U, dim / 2U); // UV.x ≈ 0.75 ⇒ right texels ⇒ green
    WARN("[texture vulkan] left R=" << (left & 0xFFU) << " G=" << ((left >> 8U) & 0xFFU) << " | right R=" << (right & 0xFFU)
                                    << " G=" << ((right >> 8U) & 0xFFU));
    CHECK((left & 0xFFU) > 200U);          // left: R high
    CHECK(((left >> 8U) & 0xFFU) < 60U);   // left: G low
    CHECK(((right >> 8U) & 0xFFU) > 200U); // right: G high
    CHECK((right & 0xFFU) < 60U);          // right: R low
}

TEST_CASE("D-007 B2-b: IR sample-op family (Lod/Grad/texelFetch/gather/textureSize) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 tw = 16U;
    crd::u8            tex_data[tw * tw * 4U];
    crd::gputest::fill_left_red_right_green(tex_data, tw, tw);
    auto texture = r.raster->create_texture(tw, tw, tex_data);
    REQUIRE(texture != nullptr);

    constexpr crd::u32 dim = 32U;
    // Build a VS+FS program, draw it textured into a fresh target, return {left, right} pixels (screen quarters).
    const auto run = [&](void (*build_fs)(kir::KGraph&, kir::KEntry&), crd::u32& left, crd::u32& right) {
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_textured_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; build_fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr); REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw_textured(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *texture, 3U);
        left  = target->read_pixel(dim / 4U, dim / 2U);
        right = target->read_pixel(3U * dim / 4U, dim / 2U);
    };

    crd::u32 l = 0;
    crd::u32 rt = 0;
    // SampleLod / SampleGrad (base level) ⇒ left red / right green.
    run(crd::gputest::build_samplelod_fs, l, rt);
    CHECK((l & 0xFFU) > 200U); CHECK(((rt >> 8U) & 0xFFU) > 200U);
    run(crd::gputest::build_samplegrad_fs, l, rt);
    CHECK((l & 0xFFU) > 200U); CHECK(((rt >> 8U) & 0xFFU) > 200U);
    // TexelFetch (integer texel) ⇒ left red / right green.
    run([](kir::KGraph& g, kir::KEntry& e) { crd::gputest::build_texelfetch_fs(g, e, tw); }, l, rt);
    CHECK((l & 0xFFU) > 200U); CHECK(((l >> 8U) & 0xFFU) < 60U); CHECK(((rt >> 8U) & 0xFFU) > 200U); CHECK((rt & 0xFFU) < 60U);
    // Gather (red channel) ⇒ left white (R=255), right black (R=0).
    run(crd::gputest::build_gather_fs, l, rt);
    WARN("[gather vulkan] left R=" << (l & 0xFFU) << " right R=" << (rt & 0xFFU));
    CHECK((l & 0xFFU) > 200U); CHECK((rt & 0xFFU) < 60U);
    // textureSize ⇒ R = G = 16 everywhere.
    run(crd::gputest::build_texsize_fs, l, rt);
    WARN("[texsize vulkan] R=" << (l & 0xFFU) << " G=" << ((l >> 8U) & 0xFFU));
    CHECK((l & 0xFFU) == tw); CHECK(((l >> 8U) & 0xFFU) == tw);
}

TEST_CASE("D-007 B2-b: IR shadow-compare sample (SampleCmp on a depth texture) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_shadow_fs(fg, fe); // sampler2DShadow, ref = uv.x, vs a 0.5 depth

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // sampler2DShadow + texture(...,vec3(uv,ref)) ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float             depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = r.raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_shadow(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);      // uv.x ≈ 0.25 ≤ 0.5 ⇒ pass ⇒ white
    const crd::u32 right = target->read_pixel(3U * dim / 4U, dim / 2U); // uv.x ≈ 0.75 > 0.5 ⇒ fail ⇒ black
    WARN("[shadow vulkan] left R=" << (left & 0xFFU) << " right R=" << (right & 0xFFU));
    CHECK((left & 0xFFU) > 200U);  // left passed the depth compare (white)
    CHECK((right & 0xFFU) < 60U);  // right failed (black)
}

TEST_CASE("D-007 B8-f: IR shadow-map foundation + bias stack renders on Vulkan", "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_shadow_foundation_fs(fg, fe);
    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float              depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = r.raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);
    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_shadow(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const auto     rc   = [](crd::u32 px) { return static_cast<int>(px & 0xFFU); };
    const crd::u32 left = target->read_pixel(7U, dim / 2U);       // fx≈7.5 → depth≈0.26 ≤ 0.5 → lit → warm
    const crd::u32 rght = target->read_pixel(24U, dim / 2U);      // fx≈24.5 → depth≈0.79 > 0.5 → shadowed → black
    WARN("[shadow-foundation vulkan] left R=" << rc(left) << " right R=" << rc(rght));
    CHECK(rc(left) > 200);  // lit
    CHECK(rc(rght) < 40);   // shadowed
}

TEST_CASE("D-007 B8-g: IR PCF filtered soft shadows render on Vulkan", "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_pcf_shadow_fs(fg, fe);
    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float              depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = r.raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);
    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_shadow(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const auto     rc   = [](crd::u32 px) { return static_cast<int>(px & 0xFFU); };
    const crd::u32 left = target->read_pixel(6U, dim / 2U);
    const crd::u32 rght = target->read_pixel(25U, dim / 2U);
    WARN("[pcf vulkan] left R=" << rc(left) << " right R=" << rc(rght));
    CHECK(rc(left) > 200);  // lit (8-tap PCF)
    CHECK(rc(rght) < 40);   // shadowed
}

TEST_CASE("D-007 B2-c: IR texture dimensions (1D/3D/Cube/2DArray/CubeArray) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 dim = 32U;
    const auto run = [&](void (*build_fs)(kir::KGraph&, kir::KEntry&), gpu::ITexture& tex) {
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_textured_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; build_fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr); REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw_textured(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, tex, 3U);
        const crd::u32 l = target->read_pixel(dim / 4U, dim / 2U);
        const crd::u32 rr = target->read_pixel(3U * dim / 4U, dim / 2U);
        CHECK((l & 0xFFU) > 200U); CHECK(((l >> 8U) & 0xFFU) < 60U);   // left red
        CHECK(((rr >> 8U) & 0xFFU) > 200U); CHECK((rr & 0xFFU) < 60U); // right green
    };

    { // 1D — 16x1 left-red/right-green
        crd::u8 d[16U * 1U * 4U];
        crd::gputest::fill_left_red_right_green(d, 16U, 1U);
        auto t = r.raster->create_texture_dim(gpu::TextureKind::Tex1D, 16U, 1U, 1U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_1d_fs, *t);
    }
    { // 3D — 16x16x2, each slice left-red/right-green
        crd::u8 d[16U * 16U * 2U * 4U];
        crd::gputest::fill_left_red_right_green(d, 16U, 16U);
        crd::gputest::fill_left_red_right_green(d + 16U * 16U * 4U, 16U, 16U);
        auto t = r.raster->create_texture_dim(gpu::TextureKind::Tex3D, 16U, 16U, 2U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_3d_fs, *t);
    }
    { // Cube — 8x8x6, faces +X,-X,+Y,-Y,+Z,-Z. dir.x<0 (screen-left) hits -X, dir.x>0 (right) hits +X ⇒ -X red · +X green.
        crd::u8 d[8U * 8U * 6U * 4U];
        crd::gputest::fill_solid(d + 0U * 64U * 4U, 64U, 0U, 255U, 0U);   // +X green (screen-right)
        crd::gputest::fill_solid(d + 1U * 64U * 4U, 64U, 255U, 0U, 0U);   // -X red   (screen-left)
        for (crd::u32 f = 2; f < 6; ++f) { crd::gputest::fill_solid(d + f * 64U * 4U, 64U, 0U, 0U, 255U); }
        auto t = r.raster->create_texture_dim(gpu::TextureKind::Cube, 8U, 8U, 6U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_cube_fs, *t);
    }
    { // 2DArray — 8x8x2: layer0 red · layer1 green
        crd::u8 d[8U * 8U * 2U * 4U];
        crd::gputest::fill_solid(d + 0U * 64U * 4U, 64U, 255U, 0U, 0U);
        crd::gputest::fill_solid(d + 1U * 64U * 4U, 64U, 0U, 255U, 0U);
        auto t = r.raster->create_texture_dim(gpu::TextureKind::Tex2DArray, 8U, 8U, 2U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_array_fs, *t);
    }
    { // CubeArray — 8x8x12: cube0 (layers 0-5) red · cube1 (6-11) green
        crd::u8 d[8U * 8U * 12U * 4U];
        crd::gputest::fill_solid(d + 0U, 6U * 64U, 255U, 0U, 0U);
        crd::gputest::fill_solid(d + 6U * 64U * 4U, 6U * 64U, 0U, 255U, 0U);
        auto t = r.raster->create_texture_dim(gpu::TextureKind::CubeArray, 8U, 8U, 12U, d);
        REQUIRE(t != nullptr);
        run(crd::gputest::build_sample_cubearray_fs, *t);
    }
}

TEST_CASE("D-007 B2-d: IR bindless texture array (dynamic index) on Vulkan", "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    if (!r.raster->supports_bindless()) { WARN("adapter has no non-uniform descriptor indexing; skipping"); return; }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_bindless_fs(fg, fe); // texture(0,3,...,8) + tex_sample_at(index = uv.x<0.5 ? 0 : 1)

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // texture2D tex[8] + nonuniformEXT indexing ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    crd::u8 red[4U * 4U * 4U];
    crd::u8 green[4U * 4U * 4U];
    crd::gputest::fill_solid(red, 16U, 255U, 0U, 0U);
    crd::gputest::fill_solid(green, 16U, 0U, 255U, 0U);
    auto t_red   = r.raster->create_texture(4U, 4U, red);
    auto t_green = r.raster->create_texture(4U, 4U, green);
    REQUIRE(t_red != nullptr);
    REQUIRE(t_green != nullptr);
    gpu::ITexture* texs[2] = {t_red.get(), t_green.get()};

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_bindless(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, texs, 2U, 3U);

    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);      // index 0 ⇒ texture[0] = red
    const crd::u32 right = target->read_pixel(3U * dim / 4U, dim / 2U); // index 1 ⇒ texture[1] = green
    WARN("[bindless vulkan] left R=" << (left & 0xFFU) << " right G=" << ((right >> 8U) & 0xFFU));
    CHECK((left & 0xFFU) > 200U);          // left: texture[0] red
    CHECK(((right >> 8U) & 0xFFU) > 200U); // right: texture[1] green
    CHECK((right & 0xFFU) < 60U);
}

TEST_CASE("D-007 B5-a: IR OpenPBR surface material writes the deferred G-buffer (MRT) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_surface_material_fs(fg, fe); // OpenPBR surface → 4 G-buffer MRT outputs

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // 4 colour outputs (MRT) ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 16U;
    auto               gbuf   = r.raster->create_gbuffer_target(dim, dim, 4U);
    REQUIRE(gbuf != nullptr);
    REQUIRE(gbuf->attachment_count() == 4U);
    r.raster->draw_gbuffer(*gbuf, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto     ch   = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    const auto     near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    const crd::u32 g0 = gbuf->read_pixel(0U, dim / 2U, dim / 2U); // (base_color, metallic)
    const crd::u32 g1 = gbuf->read_pixel(1U, dim / 2U, dim / 2U); // (normal_enc, roughness)
    const crd::u32 g2 = gbuf->read_pixel(2U, dim / 2U, dim / 2U); // (emissive, occlusion)
    const crd::u32 g3 = gbuf->read_pixel(3U, dim / 2U, dim / 2U); // (opacity, -, -, 1)
    WARN("[gbuffer vulkan] g0=" << ch(g0, 0) << "," << ch(g0, 1) << "," << ch(g0, 2) << "," << ch(g0, 3)
                                << " g1=" << ch(g1, 0) << "," << ch(g1, 2) << "," << ch(g1, 3) << " g2G=" << ch(g2, 1)
                                << " g3R=" << ch(g3, 0));
    CHECK(near(ch(g0, 0), 204)); CHECK(near(ch(g0, 1), 51)); CHECK(near(ch(g0, 2), 26)); CHECK(near(ch(g0, 3), 128)); // base+metallic
    CHECK(near(ch(g1, 0), 128)); CHECK(ch(g1, 2) > 250); CHECK(near(ch(g1, 3), 77));                                 // normal enc (0,0,1) + roughness
    CHECK(near(ch(g2, 1), 230)); CHECK(near(ch(g2, 3), 179));                                                        // emissive G + occlusion
    CHECK(ch(g3, 0) > 250);                                                                                          // opacity 1
}

TEST_CASE("D-007 B5-b: IR full OpenPBR 1.1 slab (coat/fuzz/transmission/thin-film/subsurface) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_surface_full_material_fs(fg, fe); // full OpenPBR slab → 8-attachment extended G-buffer

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // 8 colour outputs (extended MRT) ⇒ must compile to valid SPIR-V
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim  = 16U;
    auto               gbuf = r.raster->create_gbuffer_target(dim, dim, 8U);
    REQUIRE(gbuf != nullptr);
    REQUIRE(gbuf->attachment_count() == 8U);
    r.raster->draw_gbuffer(*gbuf, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch   = [&](crd::u32 att, int c) { return static_cast<int>((gbuf->read_pixel(att, dim / 2U, dim / 2U) >> (8 * c)) & 0xFFU); };
    const auto near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    WARN("[slab vulkan] spec_w=" << ch(3, 1) << " coat_w=" << ch(3, 2) << " fuzz_w=" << ch(3, 3) << " coat_b=" << ch(4, 2)
                                 << " fuzz_r=" << ch(5, 0) << " trans_w=" << ch(6, 3) << " tf_w=" << ch(7, 0)
                                 << " ss_w=" << ch(7, 2) << " thinwall=" << ch(7, 3));
    CHECK(near(ch(3, 1), 153)); CHECK(near(ch(3, 2), 102)); CHECK(near(ch(3, 3), 204)); // specular/coat/fuzz weights
    CHECK(near(ch(4, 2), 230)); CHECK(near(ch(4, 3), 51));                              // coat_color.b + coat_roughness
    CHECK(near(ch(5, 0), 230)); CHECK(near(ch(5, 3), 153));                             // fuzz_color.r + fuzz_roughness
    CHECK(near(ch(6, 1), 204)); CHECK(near(ch(6, 3), 64));                              // transmission_color.g + weight
    CHECK(near(ch(7, 0), 230)); CHECK(near(ch(7, 1), 140)); CHECK(near(ch(7, 2), 89)); CHECK(ch(7, 3) > 250); // thin-film/subsurface/thin-walled
}

TEST_CASE("D-007 B5-c: IR shading-model tag (Gooch) + masked alpha domain on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    const auto link = [&](void (*build_fs)(kir::KGraph&, kir::KEntry&)) {
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; build_fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr); REQUIRE(fs != nullptr);
        return r.raster->create_raster_program(*vs, *fs);
    };

    { // shading-model tag (Gooch = 4) flows through gbuf3.G
        auto program = link(crd::gputest::build_gooch_material_fs);
        REQUIRE(program != nullptr);
        auto gbuf = r.raster->create_gbuffer_target(16U, 16U, 4U);
        REQUIRE(gbuf != nullptr);
        r.raster->draw_gbuffer(*gbuf, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 3U);
        const int sm = static_cast<int>((gbuf->read_pixel(3U, 8U, 8U) >> 8U) & 0xFFU);
        WARN("[shading-model vulkan] gbuf3.G=" << sm << " (Gooch=4)");
        CHECK(sm == static_cast<int>(kir::material::ShadingModel::Gooch)); // exact enum value
    }
    { // masked: opacity ramp, cutoff 0.5 ⇒ left half discarded (clear), right half base_color red
        constexpr crd::u32 dim = 32U;
        kir::KGraph vg(&alloc); kir::KEntry ve; crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc); kir::KEntry fe; crd::gputest::build_masked_material_fs(fg, fe, dim);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr); REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto gbuf = r.raster->create_gbuffer_target(dim, dim, 4U);
        REQUIRE(gbuf != nullptr);
        r.raster->draw_gbuffer(*gbuf, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 3U);
        const int left  = static_cast<int>(gbuf->read_pixel(0U, dim / 4U, dim / 2U) & 0xFFU);      // opacity≈0.25<0.5 ⇒ discarded
        const int right = static_cast<int>(gbuf->read_pixel(0U, 3U * dim / 4U, dim / 2U) & 0xFFU); // opacity≈0.75 ⇒ kept (red)
        WARN("[masked vulkan] left R=" << left << " right R=" << right);
        CHECK(left < 20);    // discarded ⇒ the black clear
        CHECK(right > 200);  // kept ⇒ base_color red
    }
}

TEST_CASE("D-007 B6-a: IR MaterialX operator nodes (overlay per-channel branch) render on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][nodes]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_nodes_overlay_fs(fg, fe, dim); // overlay(fg,bg-ramp,1): branch flips at centre

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto     ch   = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    const auto     near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    const crd::u32 left  = target->read_pixel(4U, dim / 2U);  // FragCoord.x≈4.5 ⇒ bg≈0.1406<0.5 ⇒ multiply: 2*fg*bg
    const crd::u32 right = target->read_pixel(28U, dim / 2U); // FragCoord.x≈28.5 ⇒ bg≈0.8906≥0.5 ⇒ screen: 1-2*(1-bg)*(1-fg)
    WARN("[nodes overlay vulkan] left=" << ch(left, 0) << "," << ch(left, 1) << "," << ch(left, 2)
                                        << " right=" << ch(right, 0) << "," << ch(right, 1) << "," << ch(right, 2));
    // left (multiply): 2*0.8*0.1406=0.225→57 · 2*0.5*0.1406=0.141→36 · 2*0.2*0.1406=0.056→14
    CHECK(near(ch(left, 0), 57)); CHECK(near(ch(left, 1), 36)); CHECK(near(ch(left, 2), 14));
    // right (screen): 1-2*0.1094*0.2=0.956→244 · 1-2*0.1094*0.5=0.891→227 · 1-2*0.1094*0.8=0.825→210
    CHECK(near(ch(right, 0), 244)); CHECK(near(ch(right, 1), 227)); CHECK(near(ch(right, 2), 210));
}

TEST_CASE("D-007 B6-b: IR MaterialX perlin noise (U32 Bob-Jenkins hash) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][nodes][noise]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_noise_perlin_fs(fg, fe); // perlin2(FragCoord.x*scale, 0.5) → grayscale; U32 hash + logical shifts

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // the U32 hash must lower to valid SPIR-V (uint ops, logical >>)
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    // Each column x reads back the grayscale of perlin at that column — must equal the library's own F32 eval, both backends.
    int  bad = 0;
    bool any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const int got  = static_cast<int>(target->read_pixel(x, dim / 2U) & 0xFFU);
        const int want = crd::gputest::build_noise_perlin_expected(x);
        if (got < want - 4 || got > want + 4) { ++bad; }
        if (got != 128) { any = true; } // the noise actually varies (not a flat 0.5)
    }
    WARN("[noise perlin vulkan] col2 got=" << (target->read_pixel(2U, dim / 2U) & 0xFFU) << " want=" << crd::gputest::build_noise_perlin_expected(2U));
    CHECK(bad == 0);
    CHECK(any); // perlin is non-trivial across the row
}

TEST_CASE("D-007 B6-b: IR MaterialX worley (cellular) noise renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][nodes][noise]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_noise_worley_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    int  bad = 0;
    bool any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const int got  = static_cast<int>(target->read_pixel(x, dim / 2U) & 0xFFU);
        const int want = crd::gputest::build_noise_worley_expected(x);
        if (got < want - 4 || got > want + 4) { ++bad; }
        if (got != 0) { any = true; }
    }
    WARN("[noise worley vulkan] col7 got=" << (target->read_pixel(7U, dim / 2U) & 0xFFU) << " want=" << crd::gputest::build_noise_worley_expected(7U));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B6-c: IR MaterialX UV place2d (rotate2d: radians/sin/cos) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][nodes][uv]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_uv_place2d_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // radians/sin/cos must lower on the raster path
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    int  bad = 0;
    bool any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const int got  = static_cast<int>(target->read_pixel(x, dim / 2U) & 0xFFU);
        const int want = crd::gputest::build_uv_place2d_expected(x);
        if (got < want - 4 || got > want + 4) { ++bad; }
        if (got != 0 && got != 255) { any = true; }
    }
    WARN("[uv place2d vulkan] col7 got=" << (target->read_pixel(7U, dim / 2U) & 0xFFU) << " want=" << crd::gputest::build_uv_place2d_expected(7U));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B6-d: IR MaterialX NPR gooch_shade (normalize/dot/reflect/mix/pow) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][nodes][npr]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_npr_gooch_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // reflect/normalize/dot/mix/pow must lower on the raster path
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_npr_gooch_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; } // the warm/cool gradient varies across the row
    }
    WARN("[npr gooch vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                        << " want=" << crd::gputest::build_npr_gooch_expected(7U, 0) << "," << crd::gputest::build_npr_gooch_expected(7U, 1) << "," << crd::gputest::build_npr_gooch_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B7-c: IR a LOWERED material (const-fold+DCE+CSE) renders identically on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lower]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lowered_overlay_fs(fg, fe, dim); // B6 overlay material, LOWERED before create_program

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // the lowered graph must still compile to a valid program
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto     ch   = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    const auto     near = [](int got, int want) { return got >= want - 6 && got <= want + 6; };
    const crd::u32 left  = target->read_pixel(4U, dim / 2U);
    const crd::u32 right = target->read_pixel(28U, dim / 2U);
    WARN("[lowered overlay vulkan] left=" << ch(left, 0) << "," << ch(left, 1) << "," << ch(left, 2) << " right=" << ch(right, 0) << "," << ch(right, 1) << "," << ch(right, 2));
    // IDENTICAL to the un-lowered overlay material — lowering is round-trip bit-stable.
    CHECK(near(ch(left, 0), 57)); CHECK(near(ch(left, 1), 36)); CHECK(near(ch(left, 2), 14));
    CHECK(near(ch(right, 0), 244)); CHECK(near(ch(right, 1), 227)); CHECK(near(ch(right, 2), 210));
}

TEST_CASE("D-007 B8-a: IR Cook-Torrance BRDF (GGX + multiscatter) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_brdf_fs(fg, fe); // brdf_direct with a FragCoord roughness ramp

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr); // the full BRDF must lower to a valid program
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_brdf_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; } // the highlight varies across the roughness ramp
    }
    WARN("[brdf vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                   << " want=" << crd::gputest::build_lighting_brdf_expected(7U, 0) << "," << crd::gputest::build_lighting_brdf_expected(7U, 1) << "," << crd::gputest::build_lighting_brdf_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-b: IR OpenPBR lobes (clearcoat + sheen layered) render on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_layered_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_layered_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[layered vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                      << " want=" << crd::gputest::build_lighting_layered_expected(7U, 0) << "," << crd::gputest::build_lighting_layered_expected(7U, 1) << "," << crd::gputest::build_lighting_layered_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-b: IR thin-film iridescence + transmission (glass) render on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_glass_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_glass_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[glass vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                    << " want=" << crd::gputest::build_lighting_glass_expected(7U, 0) << "," << crd::gputest::build_lighting_glass_expected(7U, 1) << "," << crd::gputest::build_lighting_glass_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-c: IR punctual lights (directional + point + spot) render on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_lights_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_lights_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[lights vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                     << " want=" << crd::gputest::build_lighting_lights_expected(7U, 0) << "," << crd::gputest::build_lighting_lights_expected(7U, 1) << "," << crd::gputest::build_lighting_lights_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-d: IR area light (LTC diffuse rectangle) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_area_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_area_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[area vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                   << " want=" << crd::gputest::build_lighting_area_expected(7U, 0) << "," << crd::gputest::build_lighting_area_expected(7U, 1) << "," << crd::gputest::build_lighting_area_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-d: IR tube area light (LTC line integral) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_tube_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_tube_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[tube vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                   << " want=" << crd::gputest::build_lighting_tube_expected(7U, 0) << "," << crd::gputest::build_lighting_tube_expected(7U, 1) << "," << crd::gputest::build_lighting_tube_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-d: IR disk area light (LTC ellipse + SolveCubic) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_disk_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_disk_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[disk vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                   << " want=" << crd::gputest::build_lighting_disk_expected(7U, 0) << "," << crd::gputest::build_lighting_disk_expected(7U, 1) << "," << crd::gputest::build_lighting_disk_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-e: IR image-based lighting (SH irradiance + split-sum specular) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_ibl_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    bool       any = false;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_ibl_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        if (ch(px, 0) != ch(2U, 0)) { any = true; }
    }
    WARN("[ibl vulkan] col7 rgb=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2)
                                  << " want=" << crd::gputest::build_lighting_ibl_expected(7U, 0) << "," << crd::gputest::build_lighting_ibl_expected(7U, 1) << "," << crd::gputest::build_lighting_ibl_expected(7U, 2));
    CHECK(bad == 0);
    CHECK(any);
}

TEST_CASE("D-007 B8-h: IR cascaded shadow-map selection (split/select/snap/blend) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    constexpr crd::u32 dim = 32U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_lighting_csm_fs(fg, fe);

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_lighting_csm_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
    }
    // cascade index (R) rises left→right across the three splits: near band = cascade 0, far band = cascade 3.
    CHECK(ch(target->read_pixel(3U, dim / 2U), 0) < ch(target->read_pixel(29U, dim / 2U), 0));
    WARN("[csm vulkan] col28 rgb=" << ch(target->read_pixel(28U, dim / 2U), 0) << "," << ch(target->read_pixel(28U, dim / 2U), 1) << "," << ch(target->read_pixel(28U, dim / 2U), 2)
                                   << " want=" << crd::gputest::build_lighting_csm_expected(28U, 0) << "," << crd::gputest::build_lighting_csm_expected(28U, 1) << "," << crd::gputest::build_lighting_csm_expected(28U, 2));
    CHECK(bad == 0);
}

TEST_CASE("D-007 B8-i: IR screen-space + translucent shadows (contact / Fourier-opacity / VSM) render on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_contact_fs, crd::gputest::build_lighting_contact_expected, "contact"},
                         {crd::gputest::build_lighting_fom_fs, crd::gputest::build_lighting_fom_expected, "fom"},
                         {crd::gputest::build_lighting_vsm_fs, crd::gputest::build_lighting_vsm_expected, "vsm"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        WARN("[" << tc.tag << " vulkan] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-j: IR skinning (linear-blend + dual-quaternion) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_lbsskin_fs, crd::gputest::build_lighting_lbsskin_expected, "lbs"},
                         {crd::gputest::build_lighting_dqskin_fs, crd::gputest::build_lighting_dqskin_expected, "dquat"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        // the skinned position must vary across the sweep (the blend actually deforms) — R differs near vs far.
        CHECK(ch(target->read_pixel(3U, dim / 2U), 0) != ch(target->read_pixel(29U, dim / 2U), 0));
        WARN("[" << tc.tag << " vulkan] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-k: IR material cook seam (Forward variant renders + GBuffer variant compiles) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    // Forward variant — a material cooked into its forward pass renders LIT.
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_cook_forward_fs(fg, fe);
    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    int        bad = 0;
    for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_cook_forward_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
    }
    CHECK(ch(target->read_pixel(3U, dim / 2U), 0) != ch(target->read_pixel(29U, dim / 2U), 0)); // base_color.r sweeps → the lit red varies
    WARN("[cook-forward vulkan] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                                            << " want=" << crd::gputest::build_cook_forward_expected(27U, 0) << "," << crd::gputest::build_cook_forward_expected(27U, 1) << "," << crd::gputest::build_cook_forward_expected(27U, 2));
    CHECK(bad == 0);

    // GBuffer (deferred) variant — cooked from the SAME material, compiles to a valid program on the same backend.
    kir::KGraph gg(&alloc);
    kir::KEntry ge;
    crd::gputest::build_cook_gbuffer_fs(gg, ge);
    auto        gfs = r.ctx->create_program(gg, ge);
    CHECK(gfs != nullptr);
}

TEST_CASE("D-007 B8-l: IR render paths (deferred G-buffer lighting / clustered light-cull / decal projection) on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_deferred_fs, crd::gputest::build_lighting_deferred_expected, "deferred"},
                         {crd::gputest::build_lighting_cluster_fs, crd::gputest::build_lighting_cluster_expected, "cluster"},
                         {crd::gputest::build_lighting_decal_fs, crd::gputest::build_lighting_decal_expected, "decal"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        WARN("[" << tc.tag << " vulkan] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-m: THE CULMINATION -- skinned + textured + lit + IBL + PCF-shadowed master material on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_master_material_fs(fg, fe);
    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 tw = 16U;
    float              depth[tw * tw];
    crd::gputest::fill_uniform_depth(depth, tw, tw, 0.5F);
    auto dtex = r.raster->create_depth_texture(tw, tw, depth);
    REQUIRE(dtex != nullptr);
    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_shadow(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, *dtex, 3U);

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    // LIT region (left, receiver in front of the shadow map → shadow ≈ 1): the composed master pixel = direct + ambient, ±4.
    int bad = 0;
    for (crd::u32 x = 2U; x < 13U; x += 2U)
    {
        const crd::u32 px = target->read_pixel(x, dim / 2U);
        for (int c = 0; c < 3; ++c) { const int want = crd::gputest::build_master_lit_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
    }
    CHECK(bad == 0);
    // SHADOWED region (right): the direct term is occluded → only the IBL ambient floor remains → strictly darker than lit.
    const int lit_r = ch(target->read_pixel(6U, dim / 2U), 0);
    const int shd_r = ch(target->read_pixel(27U, dim / 2U), 0);
    WARN("[master vulkan] lit col6=" << ch(target->read_pixel(6U, dim / 2U), 0) << "," << ch(target->read_pixel(6U, dim / 2U), 1) << "," << ch(target->read_pixel(6U, dim / 2U), 2)
                                     << " (want " << crd::gputest::build_master_lit_expected(6U, 0) << "," << crd::gputest::build_master_lit_expected(6U, 1) << "," << crd::gputest::build_master_lit_expected(6U, 2) << ") shadowed col27 R=" << shd_r);
    CHECK(shd_r < lit_r - 20); // the shadow visibly darkens the direct term
    CHECK(shd_r > 0);          // ...but the ambient floor SURVIVES in shadow (not black)
}

TEST_CASE("D-007 B12: IR screen-space lighting frontier (AO/SSILVB - SSR - SSGI - volumetrics - SSS) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_ssao_fs, crd::gputest::build_lighting_ssao_expected, "ssao"},
                         {crd::gputest::build_lighting_ssr_fs, crd::gputest::build_lighting_ssr_expected, "ssr"},
                         {crd::gputest::build_lighting_ssgi_fs, crd::gputest::build_lighting_ssgi_expected, "ssgi"},
                         {crd::gputest::build_lighting_volumetric_fs, crd::gputest::build_lighting_volumetric_expected, "volumetric"},
                         {crd::gputest::build_lighting_sss_fs, crd::gputest::build_lighting_sss_expected, "sss"}};
    for (const auto& tc : cases)
    {
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        WARN("[" << tc.tag << " vulkan] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                 << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B13 post: IR HDR + TAA + bloom + cinematic + finish (specAA/CA/vignette/grain/CAS) render on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;

    const auto ch  = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
    using fs_fn    = void (*)(kir::KGraph&, kir::KEntry&);
    using exp_fn   = int (*)(crd::u32, int);
    struct Obs { fs_fn fs; exp_fn ex; const char* tag; };
    const Obs cases[] = {{crd::gputest::build_lighting_hdragx_fs, crd::gputest::build_lighting_hdragx_expected, "agx"},
                         {crd::gputest::build_lighting_hdrneutral_fs, crd::gputest::build_lighting_hdrneutral_expected, "neutral"},
                         {crd::gputest::build_lighting_hdrpq_fs, crd::gputest::build_lighting_hdrpq_expected, "pq"},
                         {crd::gputest::build_lighting_taa_fs, crd::gputest::build_lighting_taa_expected, "taa"}, // B13-a temporal resolve
                         {crd::gputest::build_lighting_bloom_fs, crd::gputest::build_lighting_bloom_expected, "bloom"}, // B13-b bloom
                         {crd::gputest::build_lighting_cine_fs, crd::gputest::build_lighting_cine_expected, "cine"}, // B13-d cinematic
                         {crd::gputest::build_lighting_finish_fs, crd::gputest::build_lighting_finish_expected, "finish"}}; // B13-e finish
    for (const auto& tc : cases)
    {
        kir::KGraph vg2(&alloc);
        kir::KEntry ve2;
        crd::gputest::build_fullscreen_vs(vg2, ve2);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        tc.fs(fg, fe);
        auto vs = r.ctx->create_program(vg2, ve2);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        int bad = 0;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = tc.ex(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
        }
        CHECK(ch(target->read_pixel(3U, dim / 2U), 0) != ch(target->read_pixel(29U, dim / 2U), 0));
        WARN("[hdr-" << tc.tag << " vulkan] col27 rgb=" << ch(target->read_pixel(27U, dim / 2U), 0) << "," << ch(target->read_pixel(27U, dim / 2U), 1) << "," << ch(target->read_pixel(27U, dim / 2U), 2)
                     << " want=" << tc.ex(27U, 0) << "," << tc.ex(27U, 1) << "," << tc.ex(27U, 2));
        CHECK(bad == 0);
    }
}

TEST_CASE("D-007 B8-d: IR area light SPECULAR (LTC LUT Minv reconstruction) renders on Vulkan",
          "[gpu-context][vulkan][gpu][raster][ir][lighting]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(8U << 20U);
    constexpr crd::u32         dim = 32U;
    kir::KGraph                vg(&alloc);
    kir::KEntry                ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    for (int which = 0; which < 2; ++which)
    {
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        if (which == 0) { crd::gputest::build_lighting_specular_fs(fg, fe); }
        else { crd::gputest::build_lighting_aniso_fs(fg, fe); }
        auto vs = r.ctx->create_program(vg, ve);
        auto fs = r.ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = r.raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = r.raster->create_color_target(dim, dim);
        REQUIRE(target != nullptr);
        r.raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        const auto ch = [](crd::u32 px, int c) { return static_cast<int>((px >> (8 * c)) & 0xFFU); };
        int        bad = 0;
        bool       any = false;
        for (crd::u32 x = 2U; x < dim - 2U; x += 5U)
        {
            const crd::u32 px = target->read_pixel(x, dim / 2U);
            for (int c = 0; c < 3; ++c) { const int want = (which == 0) ? crd::gputest::build_lighting_specular_expected(x, c) : crd::gputest::build_lighting_aniso_expected(x, c); if (ch(px, c) < want - 4 || ch(px, c) > want + 4) { ++bad; } }
            if (ch(px, 0) != ch(2U, 0)) { any = true; }
        }
        WARN("[area-spec vulkan which=" << which << "] col7=" << ch(target->read_pixel(7U, dim / 2U), 0) << "," << ch(target->read_pixel(7U, dim / 2U), 1) << "," << ch(target->read_pixel(7U, dim / 2U), 2));
        CHECK(bad == 0);
        CHECK(any);
    }
}

TEST_CASE("B-cmp: CKIR compute KERNEL (shared memory + barriers) DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(4U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              ls = 256;
    const kir::KEntry          e  = crd::kir_test::build_reverse_kernel(g, ls);

    // 1) the CPU ORACLE (f64 buffers, F32-rounded ops) — the bit-exact reference.
    crd::f64 in64[ls];
    crd::f64 out64[ls];
    for (int i = 0; i < ls; ++i) { in64[i] = 1.0 + 3.0 * static_cast<crd::f64>(i); out64[i] = -1.0; } // exact in f32
    kir::KernelBuffer bufs[2] = {{in64, ls, 0, 0}, {out64, ls, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(ls), &alloc);

    // 2) emit kernel GLSL → SPIR-V → pipeline (2 storage bindings, no push).
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_kernel", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    // 3) dispatch ONE workgroup on the portable surface, read back.
    float in32[ls];
    float out32[ls];
    for (int i = 0; i < ls; ++i) { in32[i] = static_cast<float>(in64[i]); out32[i] = -1.0F; }
    float*    host[2] = {in32, out32};
    const int lens[2] = {ls, ls};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);

    // 4) GPU == oracle, bit-for-bit (reverse is pure data movement ⇒ exact on every vendor).
    int bad = 0;
    for (int i = 0; i < ls; ++i) { if (out32[i] != static_cast<float>(out64[i])) { ++bad; } }
    CHECK(bad == 0);
    CHECK(out32[0] == static_cast<float>(in64[ls - 1])); // spot-check the reversal actually happened
    CHECK(out32[ls - 1] == static_cast<float>(in64[0]));
}

TEST_CASE("B-cmp: CKIR TRANSPOSE kernel (For loops + barrier + cross-thread) DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(4U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              t  = 8;
    constexpr int              nn = t * t;
    const kir::KEntry          e  = crd::kir_test::build_transpose_kernel(g, t);

    crd::f64 in64[nn];
    crd::f64 out64[nn];
    for (int i = 0; i < nn; ++i) { in64[i] = static_cast<crd::f64>(i); out64[i] = -1.0; }
    kir::KernelBuffer bufs[2] = {{in64, nn, 0, 0}, {out64, nn, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(t), &alloc);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_transpose", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    float in32[nn];
    float out32[nn];
    for (int i = 0; i < nn; ++i) { in32[i] = static_cast<float>(in64[i]); out32[i] = -1.0F; }
    float*    host[2] = {in32, out32};
    const int lens[2] = {nn, nn};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);

    int bad = 0;
    for (int i = 0; i < nn; ++i) { if (out32[i] != static_cast<float>(out64[i])) { ++bad; } }
    CHECK(bad == 0);
    CHECK(out32[1] == static_cast<float>(in64[t])); // out[0][1] == in[1][0] — the transpose actually happened
}

TEST_CASE("B-cmp Phase 1: CKIR radix-2 Stockham FFT DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][fft]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              n    = 64;
    constexpr int              half = n / 2;
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix2(g, n, false);

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           twr[half];
    crd::f64           twi[half];
    for (int k = 0; k < half; ++k)
    {
        const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[k]           = static_cast<crd::f64>(static_cast<float>(crd::math::cos(a)));
        twi[k]           = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(a)));
    }
    crd::f64 ir[n];
    crd::f64 ii[n];
    crd::f64 orr[n];
    crd::f64 oi[n];
    for (int i = 0; i < n; ++i)
    {
        ir[i] = static_cast<crd::f64>(static_cast<float>((i * 7 + 3) % 11 - 5));
        ii[i] = static_cast<crd::f64>(static_cast<float>((i * 5 + 1) % 7 - 3));
        orr[i] = -99.0;
        oi[i]  = -99.0;
    }
    kir::KernelBuffer bufs[6] = {{ir, n, 0, 0},   {ii, n, 0, 1},   {twr, half, 0, 2},
                                 {twi, half, 0, 3}, {orr, n, 0, 4}, {oi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, static_cast<crd::u32>(half), &alloc);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, plan.entry, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_fft",
                                                &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 6, 0U);
    REQUIRE(pipe != nullptr);

    float h_ir[n];
    float h_ii[n];
    float h_twr[half];
    float h_twi[half];
    float h_or[n];
    float h_oi[n];
    for (int i = 0; i < n; ++i) { h_ir[i] = static_cast<float>(ir[i]); h_ii[i] = static_cast<float>(ii[i]); h_or[i] = -99.0F; h_oi[i] = -99.0F; }
    for (int k = 0; k < half; ++k) { h_twr[k] = static_cast<float>(twr[k]); h_twi[k] = static_cast<float>(twi[k]); }
    float*    host[6] = {h_ir, h_ii, h_twr, h_twi, h_or, h_oi};
    const int lens[6] = {n, n, half, half, n, n};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 6, 1U);

    const auto fa = [](float x) { return x < 0.0F ? -x : x; };
    float      maxmag = 1e-6F;
    for (int k = 0; k < n; ++k) { maxmag = maxmag > fa(static_cast<float>(orr[k])) ? maxmag : fa(static_cast<float>(orr[k])); maxmag = maxmag > fa(static_cast<float>(oi[k])) ? maxmag : fa(static_cast<float>(oi[k])); }
    int   exact  = 0;
    float maxdif = 0.0F;
    for (int k = 0; k < n; ++k)
    {
        if (h_or[k] == static_cast<float>(orr[k]) && h_oi[k] == static_cast<float>(oi[k])) { ++exact; }
        const float dr = fa(h_or[k] - static_cast<float>(orr[k]));
        const float di = fa(h_oi[k] - static_cast<float>(oi[k]));
        maxdif = maxdif > dr ? maxdif : dr;
        maxdif = maxdif > di ? maxdif : di;
    }
    WARN("[fft-vk] bit-exact bins " << exact << "/" << n << "  maxdiff " << maxdif << "  maxmag " << maxmag);
    CHECK(maxdif <= 2e-3F * maxmag); // GPU FFT is correct within f32 tolerance
}

TEST_CASE("B-cmp Phase 1: CKIR RADIX-4 Stockham FFT DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][fft]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              n = 256; // power of 4; quarter = 64 threads
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix4(g, n, false);

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           twr[n]; // FULL W_N[N] table for radix-4
    crd::f64           twi[n];
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[k]           = static_cast<crd::f64>(static_cast<float>(crd::math::cos(a)));
        twi[k]           = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(a)));
    }
    crd::f64 ir[n];
    crd::f64 ii[n];
    crd::f64 orr[n];
    crd::f64 oi[n];
    for (int i = 0; i < n; ++i) { ir[i] = static_cast<crd::f64>(static_cast<float>((i * 7 + 3) % 11 - 5)); ii[i] = static_cast<crd::f64>(static_cast<float>((i * 5 + 1) % 7 - 3)); orr[i] = -99.0; oi[i] = -99.0; }
    kir::KernelBuffer bufs[6] = {{ir, n, 0, 0}, {ii, n, 0, 1}, {twr, n, 0, 2}, {twi, n, 0, 3}, {orr, n, 0, 4}, {oi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, static_cast<crd::u32>(n / 4), &alloc);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, plan.entry, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_fft4", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 6, 0U);
    REQUIRE(pipe != nullptr);

    float h_ir[n];
    float h_ii[n];
    float h_twr[n];
    float h_twi[n];
    float h_or[n];
    float h_oi[n];
    for (int i = 0; i < n; ++i) { h_ir[i] = static_cast<float>(ir[i]); h_ii[i] = static_cast<float>(ii[i]); h_twr[i] = static_cast<float>(twr[i]); h_twi[i] = static_cast<float>(twi[i]); h_or[i] = -99.0F; h_oi[i] = -99.0F; }
    float*    host[6] = {h_ir, h_ii, h_twr, h_twi, h_or, h_oi};
    const int lens[6] = {n, n, n, n, n, n};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 6, 1U);

    int bad = 0; // `precise` GLSL temps ⇒ radix-4 FFT is BIT-EXACT vs the oracle
    for (int k = 0; k < n; ++k) { if (h_or[k] != static_cast<float>(orr[k]) || h_oi[k] != static_cast<float>(oi[k])) { ++bad; } }
    CHECK(bad == 0);
}

// B-cmp crush: the REGISTER-BLOCKED radix-16 FFT (the ncu-profiled lever: 16 points/thread in registers, 3 shared
// exchanges, 64-thread blocks) DISPATCHES on Vulkan bit-exact vs the CPU oracle at n=1024 — the size the 2-D pipeline
// routes through it.
TEST_CASE("B-cmp crush: REGISTER-BLOCKED radix-16 FFT DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][fft]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              n    = 1024; // [16,16,4] stages, 64 threads
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix16(g, n, false);

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    crd::f64           twr[n];
    crd::f64           twi[n];
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[k]           = static_cast<crd::f64>(static_cast<float>(crd::math::cos(a)));
        twi[k]           = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(a)));
    }
    crd::f64 ir[n];
    crd::f64 ii[n];
    crd::f64 orr[n];
    crd::f64 oi[n];
    for (int i = 0; i < n; ++i) { ir[i] = static_cast<crd::f64>(static_cast<float>((i * 7 + 3) % 11 - 5)); ii[i] = static_cast<crd::f64>(static_cast<float>((i * 5 + 1) % 7 - 3)); orr[i] = -99.0; oi[i] = -99.0; }
    kir::KernelBuffer bufs[6] = {{ir, n, 0, 0}, {ii, n, 0, 1}, {twr, n, 0, 2}, {twi, n, 0, 3}, {orr, n, 0, 4}, {oi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, plan.entry.local_size[0], &alloc);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, plan.entry, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_fft16", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 6, 0U);
    REQUIRE(pipe != nullptr);

    float h_ir[n];
    float h_ii[n];
    float h_twr[n];
    float h_twi[n];
    float h_or[n];
    float h_oi[n];
    for (int i = 0; i < n; ++i) { h_ir[i] = static_cast<float>(ir[i]); h_ii[i] = static_cast<float>(ii[i]); h_twr[i] = static_cast<float>(twr[i]); h_twi[i] = static_cast<float>(twi[i]); h_or[i] = -99.0F; h_oi[i] = -99.0F; }
    float*    host[6] = {h_ir, h_ii, h_twr, h_twi, h_or, h_oi};
    const int lens[6] = {n, n, n, n, n, n};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 6, 1U);

    int bad = 0; // `precise` temps + table-only twiddles ⇒ the register-blocked FFT is BIT-EXACT vs the oracle
    for (int k = 0; k < n; ++k) { if (h_or[k] != static_cast<float>(orr[k]) || h_oi[k] != static_cast<float>(oi[k])) { ++bad; } }
    CHECK(bad == 0);
}

// B-cmp Phase 2: the FULL 2-D FFT — a 6-dispatch pipeline (batched row FFT -> transpose re,im -> batched col FFT ->
// transpose-back re,im) runs on Vulkan and is compared BIT-FOR-BIT to the CPU oracle driving the SAME plan. The transpose
// is pure data movement (bit-exact); each 1-D pass is `precise` radix-4 (bit-exact). One CKIR graph -> identical bits.
TEST_CASE("B-cmp Phase 2: CKIR 2-D FFT (6-dispatch pipeline) DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][fft]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g0(&alloc); // one graph per unique entry (emitter emits all of a graph's decls)
    kir::KGraph                g1(&alloc);
    kir::KGraph                g2(&alloc);
    kir::KGraph                g3(&alloc);
    kir::KGraph*               graphs[4] = {&g0, &g1, &g2, &g3};
    constexpr int              rr        = 64; // power-of-4 dims -> radix-4 row/col passes
    constexpr int              cc        = 64;
    constexpr int              tile      = 16;
    const kir::Fft2dPlan       plan      = kir::build_fft2d_c2c(graphs, rr, cc, false, tile);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[16];
    float*    h32[16];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i)
    {
        h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5); // integer -> f32-exact
        h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
    }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc); h64[plan.tw_col_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr); h64[plan.tw_row_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_row_im][k] = f32d(-crd::math::sin(a)); }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); } // f32 mirror before the oracle mutates scratch

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2d", &alloc);
        REQUIRE(spv.ok);
        pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                                            plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(compute, plan, pipes, h32);

    const auto fa     = [](float x) { return x < 0.0F ? -x : x; };
    int        badr   = 0;
    int        badi   = 0;
    float      maxdif = 0.0F;
    for (int i = 0; i < rr * cc; ++i)
    {
        const float er = static_cast<float>(h64[plan.res_re][i]);
        const float ei = static_cast<float>(h64[plan.res_im][i]);
        if (h32[plan.res_re][i] != er) { ++badr; }
        if (h32[plan.res_im][i] != ei) { ++badi; }
        maxdif = maxdif > fa(h32[plan.res_re][i] - er) ? maxdif : fa(h32[plan.res_re][i] - er);
        maxdif = maxdif > fa(h32[plan.res_im][i] - ei) ? maxdif : fa(h32[plan.res_im][i] - ei);
    }
    WARN("[fft2d-vk] " << rr << "x" << cc << " bit-exact re-bad " << badr << " im-bad " << badi << " maxdiff " << maxdif);
    CHECK(badr == 0);
    CHECK(badi == 0);
}

// B-cmp Phase 3: THE CRUSH — the FUSED 2-D FFT-convolution (7 dispatches: row FFT -> transpose -> on-chip fused column conv
// -> transpose -> inverse row FFT) on Vulkan, BIT-FOR-BIT vs the CPU oracle driving the same plan. The filter is an arbitrary
// deterministic pattern (bit-exactness is layout/emitter portability, not a specific PSF — CPU tests prove conv correctness).
TEST_CASE("B-cmp Phase 3: CKIR FUSED 2-D FFT-convolution DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][fft][conv]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator alloc(96U << 20U);
    kir::KGraph                g0(&alloc);
    kir::KGraph                g1(&alloc);
    kir::KGraph                g2(&alloc);
    kir::KGraph                g3(&alloc);
    kir::KGraph                g4(&alloc);
    kir::KGraph*               graphs[5] = {&g0, &g1, &g2, &g3, &g4};
    constexpr int              rr        = 64;
    constexpr int              cc        = 64;
    constexpr int              tile      = 16;
    const kir::Fft2dPlan       plan      = kir::build_fft2d_convolution(graphs, rr, cc, tile);

    int off[20];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> a64(&alloc);
    crd::containers::Array<float>    a32(&alloc);
    a64.resize(static_cast<crd::usize>(total), 0.0);
    a32.resize(static_cast<crd::usize>(total), 0.0F);
    crd::f64* h64[20];
    float*    h32[20];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    for (int i = 0; i < rr * cc; ++i)
    {
        h64[plan.in_re][i]   = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
        h64[plan.in_im][i]   = static_cast<crd::f64>((i * 5 + 1) % 7 - 3);
        h64[plan.filt_re][i] = f32d(static_cast<crd::f64>((i * 3 + 1) % 9 - 4) * 0.25); // arbitrary f32-exact filter
        h64[plan.filt_im][i] = f32d(static_cast<crd::f64>((i * 2 + 5) % 7 - 3) * 0.25);
    }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cc); h64[plan.tw_col_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rr); h64[plan.tw_row_re][k] = f32d(crd::math::cos(a)); h64[plan.tw_row_im][k] = f32d(-crd::math::sin(a)); }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2dconv", &alloc);
        REQUIRE(spv.ok);
        pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                                            plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(compute, plan, pipes, h32);

    int badr = 0;
    int badi = 0;
    for (int i = 0; i < rr * cc; ++i)
    {
        if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
        if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
    }
    WARN("[fft2dconv-vk] " << rr << "x" << cc << " bit-exact re-bad " << badr << " im-bad " << badi);
    CHECK(badr == 0);
    CHECK(badi == 0);
}

// The HEAD-TO-HEAD: batched radix-4 FFT dispatched over a grid of `batch` workgroups (one N-point FFT each), GPU-timed via
// `last_gpu_ms` (kernel only — upload/readback excluded, like cuFFT's cudaEvent timing). Hidden ([.fft-bench]) — run
// explicitly. Compare the printed GFLOP/s to docs/bench/2026-07-13-gpu-fft-cufft-gold.md.
TEST_CASE("B-cmp Phase 1: CKIR radix-4 BATCHED FFT -- GPU benchmark vs the cuFFT board", "[.fft-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(64U << 20U);

    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;

    for (int n : {256, 512, 1024}) // all fit one workgroup's shared (4*N floats = 16*N bytes <= 48KB); 512 = radix-8, else radix-4
    {
        const auto  bld   = [&](kir::KGraph& gg) { return n == 512 ? kir::build_fft1d_radix8(gg, n, false, true) : kir::build_fft1d_radix4(gg, n, false, true); };
        const int   batch = (16 << 20) / n; // ~16.7M complex elements total (matches the cuFFT board)
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan plan = bld(g); // batched (radix-8 for N=512, radix-4 otherwise)

        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, plan.entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fftbench", &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 6, 0U);
        REQUIRE(pipe != nullptr);

        const crd::u64 io_bytes = static_cast<crd::u64>(batch) * static_cast<crd::u64>(n) * sizeof(float);
        const crd::u64 tw_bytes = static_cast<crd::u64>(n) * sizeof(float);
        auto d_inre = compute.create_buffer(io_bytes, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_inim = compute.create_buffer(io_bytes, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_twre = compute.create_buffer(tw_bytes, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_twim = compute.create_buffer(tw_bytes, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_outre = compute.create_buffer(io_bytes, storage | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_outim = compute.create_buffer(io_bytes, storage | transfer_src, cg::ComputeMemory::GpuOnly);

        // fill + upload inputs + twiddles (staging → device, once).
        auto up = [&](cg::ComputeBuffer& dev, crd::u64 bytes, auto fill) {
            auto stg = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p  = static_cast<float*>(stg->map());
            fill(p, static_cast<int>(bytes / sizeof(float)));
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, dev, 0U, 0U, bytes);
            compute.submit_and_wait();
        };
        up(*d_inre, io_bytes, [&](float* p, int cnt) { for (int i = 0; i < cnt; ++i) { p[i] = static_cast<float>((i * 7 + 3) % 11 - 5); } });
        up(*d_inim, io_bytes, [&](float* p, int cnt) { for (int i = 0; i < cnt; ++i) { p[i] = static_cast<float>((i * 5 + 1) % 7 - 3); } });
        up(*d_twre, tw_bytes, [&](float* p, int cnt) { for (int k = 0; k < cnt; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(*d_twim, tw_bytes, [&](float* p, int cnt) { for (int k = 0; k < cnt; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });

        cg::ComputeBuffer* binds[6] = {d_inre.get(), d_inim.get(), d_twre.get(), d_twim.get(), d_outre.get(), d_outim.get()};
        double             best     = 1e30;
        for (int r = 0; r < 30; ++r) // min-of-30, kernel-only (last_gpu_ms brackets just the recorded dispatch)
        {
            auto& rec = compute.begin();
            rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 6), nullptr, 0U, static_cast<crd::u32>(batch), 1U, 1U);
            compute.submit_and_wait();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < best) { best = ms; }
        }
        const double gflops = 5.0 * n * static_cast<double>(plan.log2n) * static_cast<double>(batch) / (best * 1e-3) / 1e9;

        // HONESTY GATE: a fast WRONG kernel is meaningless — verify wg0's output is a correct bit-exact FFT vs the oracle.
        auto rb_re = compute.create_buffer(tw_bytes, transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto rb_im = compute.create_buffer(tw_bytes, transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*d_outre, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.barrier(*d_outim, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*d_outre, *rb_re, 0U, 0U, tw_bytes);
            rec.copy(*d_outim, *rb_im, 0U, 0U, tw_bytes);
            compute.submit_and_wait();
        }
        kir::KGraph          go(&alloc);
        const kir::Fft1dPlan plano = bld(go);
        crd::containers::Array<crd::f64> ir0(&alloc); crd::containers::Array<crd::f64> ii0(&alloc);
        crd::containers::Array<crd::f64> tr0(&alloc); crd::containers::Array<crd::f64> ti0(&alloc);
        crd::containers::Array<crd::f64> or0(&alloc); crd::containers::Array<crd::f64> oi0(&alloc);
        ir0.resize(static_cast<crd::usize>(n)); ii0.resize(static_cast<crd::usize>(n));
        tr0.resize(static_cast<crd::usize>(n)); ti0.resize(static_cast<crd::usize>(n));
        or0.resize(static_cast<crd::usize>(n)); oi0.resize(static_cast<crd::usize>(n));
        for (int i = 0; i < n; ++i)
        {
            ir0[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>((i * 7 + 3) % 11 - 5));
            ii0[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>((i * 5 + 1) % 7 - 3));
            tr0[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(two_pi * i / n)));
            ti0[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(two_pi * i / n)));
            or0[static_cast<crd::usize>(i)] = -99.0; oi0[static_cast<crd::usize>(i)] = -99.0;
        }
        kir::KernelBuffer ob[6] = {{ir0.data(), n, 0, 0}, {ii0.data(), n, 0, 1}, {tr0.data(), n, 0, 2},
                                   {ti0.data(), n, 0, 3}, {or0.data(), n, 0, 4}, {oi0.data(), n, 0, 5}};
        kir::eval_cpu_kernel(go, plano.entry, ob, 6, plano.entry.local_size[0], &alloc, 1U); // wg0 (n/4 radix-4, n/8 radix-8)
        const auto* gr = static_cast<const float*>(rb_re->map());
        const auto* gi = static_cast<const float*>(rb_im->map());
        int         bad = 0;
        for (int i = 0; i < n; ++i) { if (gr[i] != static_cast<float>(or0[static_cast<crd::usize>(i)]) || gi[i] != static_cast<float>(oi0[static_cast<crd::usize>(i)])) { ++bad; } }
        rb_re->unmap();
        rb_im->unmap();

        WARN("[fft-vk-bench] N=" << n << " batch=" << batch << " min_ms=" << best << " GFLOP/s=" << gflops
                                 << "  wg0 bit-exact-vs-oracle=" << (bad == 0));
        CHECK(best < 1e29); // a real timing was captured
        CHECK(bad == 0);    // the benchmarked kernel computes a CORRECT bit-exact FFT
    }
}

// ⭐ THE CRUSH: our FUSED FFT-convolution (fwd→×spectrum→iFFT in ONE on-chip dispatch) vs the vendor's THREE global passes
// (cufft_conv_bench.exe ~0.88 ms). Batched, GPU-timed kernel-only, self-verifying (identity filter Filt=1 ⇒ out≈in). Hidden.
TEST_CASE("B-cmp Phase 3: CKIR FUSED FFT-convolution -- GPU benchmark vs cuFFT's 3-pass", "[.fft-conv-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(64U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;

    for (int n : {256, 1024})
    {
        const int            batch = (16 << 20) / n;
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan plan = kir::build_fft1d_convolution(g, n, true); // batched
        kir::GlslKernel      kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, plan.entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fftconv", &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 8, 0U);
        REQUIRE(pipe != nullptr);

        const crd::u64 io = static_cast<crd::u64>(batch) * static_cast<crd::u64>(n) * sizeof(float);
        const crd::u64 nb = static_cast<crd::u64>(n) * sizeof(float);
        auto d_inre  = compute.create_buffer(io, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_inim  = compute.create_buffer(io, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_twre  = compute.create_buffer(nb, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_twim  = compute.create_buffer(nb, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_ftre  = compute.create_buffer(nb, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_ftim  = compute.create_buffer(nb, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
        auto d_outre = compute.create_buffer(io, storage | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_outim = compute.create_buffer(io, storage | transfer_src, cg::ComputeMemory::GpuOnly);
        auto up = [&](cg::ComputeBuffer& dev, crd::u64 bytes, auto fill) {
            auto  stg = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p   = static_cast<float*>(stg->map());
            fill(p, static_cast<int>(bytes / sizeof(float)));
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, dev, 0U, 0U, bytes);
            compute.submit_and_wait();
        };
        up(*d_inre, io, [&](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = static_cast<float>((i * 7 + 3) % 11 - 5); } });
        up(*d_inim, io, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });
        up(*d_twre, nb, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(*d_twim, nb, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(*d_ftre, nb, [](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = 1.0F; } }); // identity filter (Filt = FFT(delta) = 1)
        up(*d_ftim, nb, [](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = 0.0F; } });

        cg::ComputeBuffer* binds[8] = {d_inre.get(), d_inim.get(), d_twre.get(), d_twim.get(),
                                       d_ftre.get(), d_ftim.get(), d_outre.get(), d_outim.get()};
        double             best     = 1e30;
        for (int r = 0; r < 30; ++r)
        {
            auto& rec = compute.begin();
            rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 8), nullptr, 0U, static_cast<crd::u32>(batch), 1U, 1U);
            compute.submit_and_wait();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < best) { best = ms; }
        }
        // verify wg0: identity filter ⇒ out ≈ in (round-trip FFT/iFFT, f32 tol).
        auto rb = compute.create_buffer(nb, transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*d_outre, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*d_outre, *rb, 0U, 0U, nb);
            compute.submit_and_wait();
        }
        const auto* o   = static_cast<const float*>(rb->map());
        const auto  fa  = [](float x) { return x < 0.0F ? -x : x; };
        int         bad = 0;
        for (int i = 0; i < n; ++i) { if (fa(o[i] - static_cast<float>((i * 7 + 3) % 11 - 5)) > 2e-3F * 6.0F) { ++bad; } }
        rb->unmap();

        const double cufft_conv = (n == 256) ? 0.879 : 0.887; // cufft_conv_bench.exe on this GPU
        WARN("[fft-conv-bench] N=" << n << " OURS(fused 1-dispatch) " << best << " ms  vs cuFFT(3-pass) " << cufft_conv
                                   << " ms  = " << (cufft_conv / best) << "x  identity-recovers-input=" << (bad == 0));
        CHECK(best < 1e29);
        CHECK(bad == 0);
    }
}

// THE 2-D CRUSH BOARD: the FUSED 2-D FFT-convolution (7 dispatches, all kernel time) vs cuFFT's 3-pass 2-D conv, per image.
// GPU-timed (last_gpu_ms brackets the whole 7-dispatch command buffer; upload excluded, like cuFFT's cudaEvent). Hidden
// ([.fft2dconv-bench]) — run explicitly; compare to docs/bench/2026-07-13-gpu-fft-cufft-gold.md + cufft_2d_conv_bench.exe.
TEST_CASE("B-cmp Phase 3: CKIR FUSED 2-D FFT-convolution -- GPU benchmark vs cuFFT's 3-pass 2-D", "[.fft2dconv-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;

    for (int n : {256, 1024}) // power-of-4 square images (our fused conv is radix-4)
    {
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph          g2(&alloc);
        kir::KGraph          g3(&alloc);
        kir::KGraph          g4(&alloc);
        kir::KGraph*         graphs[5] = {&g0, &g1, &g2, &g3, &g4};
        const int            tile      = (n >= 1024) ? 32 : 16;
        const kir::Fft2dPlan plan      = kir::build_fft2d_convolution(graphs, n, n, tile);

        std::unique_ptr<cg::ComputePipeline> pipe_store[8];
        cg::ComputePipeline*                 pipes[8] = {};
        for (int pi = 0; pi < plan.npasses; ++pi)
        {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2dconv", &alloc);
            REQUIRE(spv.ok);
            pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), plan.passes[pi].nbind, 0U);
            REQUIRE(pipe_store[pi] != nullptr);
            pipes[pi] = pipe_store[pi].get();
        }

        std::unique_ptr<cg::ComputeBuffer> dev[20];
        for (int b = 0; b < plan.nbuffers; ++b)
        {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[b].size) * sizeof(float);
            dev[b] = compute.create_buffer(bytes, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        }
        auto up = [&](int id, auto fill) {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[id].size) * sizeof(float);
            auto           stg   = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto*          p     = static_cast<float*>(stg->map());
            fill(p, plan.buffers[id].size);
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *dev[id], 0U, 0U, bytes);
            compute.submit_and_wait();
        };
        up(plan.in_re, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = static_cast<float>((i * 7 + 3) % 11 - 5); } });
        up(plan.in_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });
        up(plan.tw_col_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_col_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.tw_row_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_row_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.filt_re, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 1.0F; } }); // identity filter (round-trip)
        up(plan.filt_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });

        auto record = [&]() {
            auto& rec = compute.begin();
            for (int pi = 0; pi < plan.npasses; ++pi)
            {
                const kir::Fft2dPass& p        = plan.passes[pi];
                cg::ComputeBuffer*    binds[8] = {};
                for (int k = 0; k < p.nbind; ++k) { binds[k] = dev[p.bind[k]].get(); }
                rec.dispatch(*pipes[pi], crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(p.nbind)), nullptr, 0U, p.num_workgroups, 1U, 1U);
                if (pi + 1 < plan.npasses) // barrier ONLY the buffers this pass WROTE (last 1 for transpose, last 2 for FFT/conv)
                {
                    const int nout = (p.nbind == 2) ? 1 : 2;
                    for (int j = p.nbind - nout; j < p.nbind; ++j) { rec.barrier(*dev[p.bind[j]], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); }
                }
            }
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); } // warmup
        double best = 1e30;
        for (int r = 0; r < 30; ++r)
        {
            record();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < best) { best = ms; }
        }

        // SELF-CHECK: identity filter (H = 1) ⇒ the whole pipeline is FFT2 → identity → IFFT2 ⇒ out == in (f32 tol).
        {
            const crd::u64 bytes = static_cast<crd::u64>(n) * sizeof(float); // row 0 of res_re
            auto           rb    = compute.create_buffer(bytes, transfer_dst, cg::ComputeMemory::GpuToCpu);
            auto&          rec   = compute.begin();
            rec.barrier(*dev[plan.res_re], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*dev[plan.res_re], *rb, 0U, 0U, bytes);
            compute.submit_and_wait();
            const auto* o   = static_cast<const float*>(rb->map());
            const auto  fa  = [](float x) { return x < 0.0F ? -x : x; };
            int         bad = 0;
            for (int i = 0; i < n; ++i) { if (fa(o[i] - static_cast<float>((i * 7 + 3) % 11 - 5)) > 5e-3F * 6.0F) { ++bad; } }
            rb->unmap();
            CHECK(bad == 0); // the measured pipeline RECOVERS the input — the run is real, not garbage-fast
        }
        const double cufft = (n == 256) ? 0.0357 : 0.0473; // cufft_2d_conv_bench.exe on this GPU (batch=1)
        WARN("[fft2dconv-bench] N=" << n << "x" << n << " OURS(7-dispatch fused) " << best << " ms  vs cuFFT(3-pass 2-D) "
                                    << cufft << " ms  = " << (cufft / best) << "x");
        CHECK(best < 1e29);
    }
}

// TILED COLUMN FFT — does coalesced tiling (tile_c adjacent columns per block) beat the uncoalesced strided column FFT?
// Times the 1024-pt column FFT over a 1024² image at tile_c = 1 (strided) vs 2,4 (tiled), and checks every tile_c gives the
// SAME output (each column's FFT is independent of the thread grouping ⇒ bit-identical). Hidden ([.fft-tiledcol]).
TEST_CASE("B-cmp Phase 3: CKIR tiled column FFT -- coalescing benchmark", "[.fft-tiledcol]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;
    constexpr int      n      = 1024;
    const crd::u64     ib     = static_cast<crd::u64>(n) * n * sizeof(float); // image plane
    const crd::u64     nb     = static_cast<crd::u64>(n) * sizeof(float);     // twiddle

    auto d_in_re = compute.create_buffer(ib, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_in_im = compute.create_buffer(ib, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_tw_re = compute.create_buffer(nb, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_tw_im = compute.create_buffer(nb, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out_re = compute.create_buffer(ib, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_out_im = compute.create_buffer(ib, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto up = [&](cg::ComputeBuffer& dev, crd::u64 bytes, auto fill) {
        auto  stg = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<float*>(stg->map());
        fill(p, static_cast<int>(bytes / sizeof(float)));
        stg->unmap();
        auto& rec = compute.begin();
        rec.copy(*stg, dev, 0U, 0U, bytes);
        compute.submit_and_wait();
    };
    up(*d_in_re, ib, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = static_cast<float>((i * 7 + 3) % 11 - 5); } });
    up(*d_in_im, ib, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });
    up(*d_tw_re, nb, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
    up(*d_tw_im, nb, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });

    float ref0[64];
    for (int tc : {1, 2, 4})
    {
        kir::KGraph          g(&alloc);
        const kir::Fft1dPlan plan = kir::build_fft1d_radix16(g, n, false, true, tc, n); // col FFT of a row-major image
        kir::GlslKernel      kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, plan.entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "tiledcol", &alloc);
        if (!spv.ok) { WARN("[fft-tiledcol] tile_c=" << tc << " compile FAILED (shared limit?): " << spv.error_message.c_str()); continue; }
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 6, 0U);
        if (pipe == nullptr) { WARN("[fft-tiledcol] tile_c=" << tc << " pipeline FAILED (shared limit)"); continue; }

        cg::ComputeBuffer* binds[6] = {d_in_re.get(), d_in_im.get(), d_tw_re.get(), d_tw_im.get(), d_out_re.get(), d_out_im.get()};
        const crd::u32     grid     = static_cast<crd::u32>(n / tc);
        auto               run      = [&]() { auto& rec = compute.begin(); rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 6), nullptr, 0U, grid, 1U, 1U); compute.submit_and_wait(); };
        for (int w = 0; w < 5; ++w) { run(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { run(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        auto rb = compute.create_buffer(64U * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        { auto& rec = compute.begin(); rec.barrier(*d_out_re, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc); rec.copy(*d_out_re, *rb, 0U, 0U, 64U * sizeof(float)); compute.submit_and_wait(); }
        const auto* o = static_cast<const float*>(rb->map());
        int         bad = 0;
        if (tc == 1) { for (int i = 0; i < 64; ++i) { ref0[i] = o[i]; } }
        else { for (int i = 0; i < 64; ++i) { if (o[i] != ref0[i]) { ++bad; } } }
        rb->unmap();
        WARN("[fft-tiledcol] tile_c=" << tc << "  " << best << " ms  matches-tile1=" << (tc == 1 ? 1 : (bad == 0)));
        CHECK(best < 1e29);
        if (tc != 1) { CHECK(bad == 0); } // tiling must not change the result
    }
}

// THE TRANSPOSE-ON-WRITE BOARD: the 3-dispatch strided conv (row FFT -> strided in-place column conv -> inv row FFT, NO
// transpose passes) vs the 7-dispatch coalesced-transpose conv. Measures the trade: 4 fewer passes vs uncoalesced column
// access. Identity filter ⇒ the run self-verifies (out == in). Hidden ([.fft2dconv-strided]).
TEST_CASE("B-cmp Phase 3: CKIR TRANSPOSE-ON-WRITE 3-dispatch conv -- GPU benchmark vs the 7-dispatch", "[.fft2dconv-strided]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;

    const int nt_cases[2][2] = {{1024, 1}, {1024, 4}}; // (N, tile_c): strided baseline vs tiled coalesced. tile_c=4 (32 KB
    for (int ci = 0; ci < 2; ++ci)                      // shared) is the max at 8 KB/col; tile_c=8 needs the 4 KB time-mux exchange.
    {
        const int            n  = nt_cases[ci][0];
        const int            tc = nt_cases[ci][1];
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph          g2(&alloc);
        kir::KGraph*         graphs[3] = {&g0, &g1, &g2};
        const kir::Fft2dPlan plan      = kir::build_fft2d_convolution_strided(graphs, n, n, tc);

        std::unique_ptr<cg::ComputePipeline> pipe_store[8];
        cg::ComputePipeline*                 pipes[8] = {};
        bool                                 ok = true;
        for (int pi = 0; pi < plan.npasses; ++pi)
        {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2dconvs", &alloc);
            if (!spv.ok) { ok = false; break; }
            pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), plan.passes[pi].nbind, 0U);
            if (pipe_store[pi] == nullptr) { ok = false; break; }
            pipes[pi] = pipe_store[pi].get();
        }
        if (!ok) { WARN("[fft2dconv-strided] N=" << n << " tile_c=" << tc << " SKIPPED (shared over device limit)"); continue; }

        std::unique_ptr<cg::ComputeBuffer> dev[16];
        for (int b = 0; b < plan.nbuffers; ++b)
        {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[b].size) * sizeof(float);
            dev[b] = compute.create_buffer(bytes, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        }
        auto up = [&](int id, auto fill) {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[id].size) * sizeof(float);
            auto           stg   = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto*          p     = static_cast<float*>(stg->map());
            fill(p, plan.buffers[id].size);
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *dev[id], 0U, 0U, bytes);
            compute.submit_and_wait();
        };
        up(plan.in_re, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = static_cast<float>((i * 7 + 3) % 11 - 5); } });
        up(plan.in_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });
        up(plan.tw_col_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_col_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.tw_row_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_row_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.filt_re, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 1.0F; } }); // identity ⇒ out == in
        up(plan.filt_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });

        auto record = [&]() {
            auto& rec = compute.begin();
            for (int pi = 0; pi < plan.npasses; ++pi)
            {
                const kir::Fft2dPass& p        = plan.passes[pi];
                cg::ComputeBuffer*    binds[8] = {};
                for (int k = 0; k < p.nbind; ++k) { binds[k] = dev[p.bind[k]].get(); }
                rec.dispatch(*pipes[pi], crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(p.nbind)), nullptr, 0U, p.num_workgroups, 1U, 1U);
                if (pi + 1 < plan.npasses)
                {
                    const int nout = 2;
                    for (int j = p.nbind - nout; j < p.nbind; ++j) { rec.barrier(*dev[p.bind[j]], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); }
                }
            }
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        auto rb = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*dev[plan.res_re], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*dev[plan.res_re], *rb, 0U, 0U, static_cast<crd::u64>(n) * sizeof(float));
            compute.submit_and_wait();
        }
        const auto* o   = static_cast<const float*>(rb->map());
        const auto  fa  = [](float x) { return x < 0.0F ? -x : x; };
        int         bad = 0;
        for (int i = 0; i < n; ++i) { if (fa(o[i] - static_cast<float>((i * 7 + 3) % 11 - 5)) > 5e-3F * 6.0F) { ++bad; } }
        rb->unmap();

        const double cufft = 0.0473;
        WARN("[fft2dconv-strided] N=" << n << " tile_c=" << tc << " OURS(3-dispatch) " << best << " ms vs cuFFT " << cufft
                                      << " = " << (cufft / best) << "x  identity-recovers-input=" << (bad == 0));
        CHECK(best < 1e29);
        CHECK(bad == 0); // the transpose-on-write pipeline is CORRECT (round-trip recovers the input)
    }
}

// ⭐⭐ THE DRAM-BOUND CRUSH — B contiguous images share ONE PSF spectrum. cuFFT's per-image time TRIPLES once B·8 MB spills L2
// (measured RTX 4070 Ti SUPER: 0.037 ms/img at B=4 L2-resident → 0.115 ms/img at B>=8 DRAM-bound). Our FUSED pipeline moves
// ~56 MB/image vs cuFFT's ~88 MB (3 global passes vs ~5), so DRAM-bound our fewer round-trips WIN — the 2-D analogue of the
// 1-D 1.99× crush. Identity filter ⇒ per-image round-trip recovers the input (self-verifying). Hidden ([.fft2dconv-batched]).
// Compare per_image_ms to cufft_2d_conv_batched_bench.exe + docs/bench/2026-07-13-gpu-fft-cufft-gold.md.
TEST_CASE("B-cmp Phase 3: CKIR BATCHED fused 2-D conv -- the DRAM-bound crush vs cuFFT batched", "[.fft2dconv-batched]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;

    const int    n            = 1024;
    const int    tc           = 4;
    const int    batches[4]   = {1, 4, 8, 16}; // B>=8 is the DRAM-bound crush regime (L2 spills at 8 MB/image)
    const double cufft_pi[4]  = {0.04813, 0.03717, 0.11392, 0.11518}; // per-image gold (cufft_2d_conv_batched_bench.exe)
    const int    rc           = n * n;
    for (int ci = 0; ci < 4; ++ci)
    {
        const int            bt = batches[ci];
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph          g2(&alloc);
        kir::KGraph*         graphs[3] = {&g0, &g1, &g2};
        const kir::Fft2dPlan plan      = kir::build_fft2d_convolution_strided(graphs, n, n, tc, bt);

        std::unique_ptr<cg::ComputePipeline> pipe_store[8];
        cg::ComputePipeline*                 pipes[8] = {};
        bool                                 ok = true;
        for (int pi = 0; pi < plan.npasses; ++pi)
        {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2dconvb", &alloc);
            if (!spv.ok) { ok = false; break; }
            pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), plan.passes[pi].nbind, 0U);
            if (pipe_store[pi] == nullptr) { ok = false; break; }
            pipes[pi] = pipe_store[pi].get();
        }
        if (!ok) { WARN("[fft2dconv-batched] B=" << bt << " SKIPPED (compile/pipeline)"); continue; }

        std::unique_ptr<cg::ComputeBuffer> dev[16];
        for (int b = 0; b < plan.nbuffers; ++b)
        {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[b].size) * sizeof(float);
            dev[b] = compute.create_buffer(bytes, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        }
        auto up = [&](int id, auto fill) {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[id].size) * sizeof(float);
            auto           stg   = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto*          p     = static_cast<float*>(stg->map());
            fill(p, plan.buffers[id].size);
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *dev[id], 0U, 0U, bytes);
            compute.submit_and_wait();
        };
        // per-image VARIED input ⇒ a cross-image index bug corrupts the self-check
        up(plan.in_re, [&](float* p, int c) { for (int j = 0; j < c; ++j) { const int b = j / rc; const int i = j % rc; p[j] = static_cast<float>((i * 7 + 3 + b * 13) % 11 - 5); } });
        up(plan.in_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });
        up(plan.tw_col_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_col_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.tw_row_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_row_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.filt_re, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 1.0F; } }); // identity spectrum ⇒ out == in
        up(plan.filt_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });

        auto record = [&]() {
            auto& rec = compute.begin();
            for (int pi = 0; pi < plan.npasses; ++pi)
            {
                const kir::Fft2dPass& p        = plan.passes[pi];
                cg::ComputeBuffer*    binds[8] = {};
                for (int k = 0; k < p.nbind; ++k) { binds[k] = dev[p.bind[k]].get(); }
                rec.dispatch(*pipes[pi], crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(p.nbind)), nullptr, 0U, p.num_workgroups, 1U, 1U);
                if (pi + 1 < plan.npasses)
                {
                    const int nout = 2;
                    for (int j = p.nbind - nout; j < p.nbind; ++j) { rec.barrier(*dev[p.bind[j]], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); }
                }
            }
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        const crd::u64 rbbytes = static_cast<crd::u64>(plan.buffers[plan.res_re].size) * sizeof(float);
        auto           rb      = compute.create_buffer(rbbytes, transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*dev[plan.res_re], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*dev[plan.res_re], *rb, 0U, 0U, rbbytes);
            compute.submit_and_wait();
        }
        const auto* o   = static_cast<const float*>(rb->map());
        const auto  fa  = [](float x) { return x < 0.0F ? -x : x; };
        int         bad = 0;
        for (int j = 0; j < plan.buffers[plan.res_re].size; ++j)
        {
            const int b = j / rc;
            const int i = j % rc;
            if (fa(o[j] - static_cast<float>((i * 7 + 3 + b * 13) % 11 - 5)) > 5e-3F * 12.0F) { ++bad; }
        }
        rb->unmap();

        const double per_image = best / bt;
        const double gold      = cufft_pi[ci];
        WARN("[fft2dconv-batched] B=" << bt << " OURS " << best << " ms (" << per_image << " ms/img) vs cuFFT " << gold
                                      << " ms/img = " << (gold / per_image) << "x  identity-recovers-input=" << (bad == 0));
        CHECK(best < 1e29);
        CHECK(bad == 0);
    }
}

// ⭐⭐⭐ THE REAL-FFT CRUSH — a REAL image + REAL PSF has a Hermitian spectrum, so the column conv is HALF-WIDTH (Wp = cols/2+1
// padded) ⇒ ~half the traffic + column work vs the full-complex batched conv. R2C row FFT (real→half) → half-width tiled
// column conv → C2R row FFT (half→real). Identity filter (H=1) ⇒ per-image round-trip recovers the input. Hidden
// ([.fft2dconv-r2c]). Compare per_image_ms to cufft_2d_conv_r2c_batched_bench.exe (the vendor's REAL 2-D conv).
TEST_CASE("B-cmp Phase 3: CKIR R2C REAL batched 2-D conv -- the half-spectrum crush vs cuFFT R2C", "[.fft2dconv-r2c]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    constexpr crd::f64 two_pi = 6.28318530717958647693;

    const int    n          = 1024;
    const int    tc         = 4;
    const int    batches[4] = {4, 8, 16, 32};                          // 4 = L2-resident, 16+ = DRAM-bound crush
    const double cufft_pi[4] = {0.02651, 0.03315, 0.05588, 0.05624};   // cuFFT R2C gold (cufft_2d_conv_r2c_batched_bench.exe)
    const int    rc         = n * n;
    for (int ci = 0; ci < 4; ++ci)
    {
        const int            bt = batches[ci];
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph          g2(&alloc);
        kir::KGraph*         graphs[3] = {&g0, &g1, &g2};
        const kir::Fft2dPlan plan      = kir::build_fft2d_convolution_r2c(graphs, n, n, tc, bt);
        const int            hw        = plan.buffers[static_cast<crd::usize>(plan.filt_re)].size / n;

        std::unique_ptr<cg::ComputePipeline> pipe_store[8];
        cg::ComputePipeline*                 pipes[8] = {};
        bool                                 ok = true;
        for (int pi = 0; pi < plan.npasses; ++pi)
        {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2dr2c", &alloc);
            if (!spv.ok) { WARN("[fft2dconv-r2c] pass " << pi << " GLSL compile FAILED: " << spv.error_message.c_str()); ok = false; break; }
            pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), plan.passes[pi].nbind, 0U);
            if (pipe_store[pi] == nullptr) { ok = false; break; }
            pipes[pi] = pipe_store[pi].get();
        }
        if (!ok) { WARN("[fft2dconv-r2c] bt=" << bt << " SKIPPED"); continue; }

        std::unique_ptr<cg::ComputeBuffer> dev[20];
        for (int b = 0; b < plan.nbuffers; ++b)
        {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[b].size) * sizeof(float);
            dev[b] = compute.create_buffer(bytes, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        }
        auto up = [&](int id, auto fill) {
            const crd::u64 bytes = static_cast<crd::u64>(plan.buffers[id].size) * sizeof(float);
            auto           stg   = compute.create_buffer(bytes, transfer_src, cg::ComputeMemory::CpuToGpu);
            auto*          p     = static_cast<float*>(stg->map());
            fill(p, plan.buffers[id].size);
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *dev[id], 0U, 0U, bytes);
            compute.submit_and_wait();
        };
        up(plan.in_re, [&](float* p, int c) { for (int j = 0; j < c; ++j) { const int b = j / rc; const int i = j % rc; p[j] = static_cast<float>((i * 7 + 3 + b * 13) % 11 - 5); } });
        up(plan.tw_col_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_col_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.tw_row_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_row_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.filt_re, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 1.0F; } }); // identity spectrum ⇒ out == in
        up(plan.filt_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });

        auto record = [&]() {
            auto& rec = compute.begin();
            for (int pi = 0; pi < plan.npasses; ++pi)
            {
                const kir::Fft2dPass& p        = plan.passes[pi];
                cg::ComputeBuffer*    binds[8] = {};
                for (int k = 0; k < p.nbind; ++k) { binds[k] = dev[p.bind[k]].get(); }
                rec.dispatch(*pipes[pi], crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(p.nbind)), nullptr, 0U, p.num_workgroups, 1U, 1U);
                if (pi + 1 < plan.npasses)
                {
                    const int nout = 2;
                    for (int j = p.nbind - nout; j < p.nbind; ++j) { rec.barrier(*dev[p.bind[j]], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); }
                }
            }
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        const crd::u64 rbbytes = static_cast<crd::u64>(plan.buffers[plan.res_re].size) * sizeof(float);
        auto           rb      = compute.create_buffer(rbbytes, transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*dev[plan.res_re], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*dev[plan.res_re], *rb, 0U, 0U, rbbytes);
            compute.submit_and_wait();
        }
        const auto* o   = static_cast<const float*>(rb->map());
        const auto  fa  = [](float x) { return x < 0.0F ? -x : x; };
        int         bad = 0;
        for (int j = 0; j < plan.buffers[plan.res_re].size; ++j)
        {
            const int b = j / rc;
            const int i = j % rc;
            if (fa(o[j] - static_cast<float>((i * 7 + 3 + b * 13) % 11 - 5)) > 5e-3F * 12.0F) { ++bad; }
        }
        rb->unmap();

        const double per_image = best / bt;
        const double gold      = cufft_pi[ci];
        WARN("[fft2dconv-r2c] B=" << bt << " OURS " << best << " ms (" << per_image << " ms/img) vs cuFFT-R2C " << gold
                                  << " = " << (gold / per_image) << "x  hw=" << hw << "  identity-recovers-input=" << (bad == 0));
        CHECK(best < 1e29);
        CHECK(bad == 0);
    }
}

// B-cmp: the CKIR device-wide REDUCTION (ckir_reduce.hpp) dispatches bit-exact on Vulkan vs the CPU oracle — the 2-pass
// plan (grid of blocks → partials → final workgroup). Sum is bit-exact (fixed serial+tree order + `precise` temps); max is
// order-invariant. Both passes drive through the shared `dispatch_kernel_1wg` harness (partials round-trip via host).
TEST_CASE("B-cmp: CKIR device REDUCTION DISPATCHES on Vulkan == CPU oracle bit-exact", "[gpu-context][vulkan][gpu][kernel][reduce]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    constexpr int n = 65536;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f64> x64(&alloc);
    crd::containers::Array<float>    x32(&alloc);
    x64.resize(n); x32.resize(n);
    for (int i = 0; i < n; ++i) { x64[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 31 + 5) % 251 - 125); x32[static_cast<crd::usize>(i)] = static_cast<float>(x64[static_cast<crd::usize>(i)]); }

    const kir::KOp ops[2] = {kir::KOp::Add, kir::KOp::Max};
    for (int oi = 0; oi < 2; ++oi)
    {
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph*         graphs[2] = {&g0, &g1};
        const kir::ReducePlan plan     = kir::build_reduce(graphs, n, ops[oi], 256, 64);
        REQUIRE_FALSE(plan.single_pass);

        // CPU oracle: pass 0 (grid=nblocks) → partials, pass 1 (grid=1) → out.
        crd::containers::Array<crd::f64> part64(&alloc); part64.resize(static_cast<crd::usize>(plan.nblocks), 0.0);
        crd::f64                         out64 = -1234.0;
        kir::KernelBuffer kb0[2] = {{x64.data(), n, 0, 0}, {part64.data(), plan.nblocks, 0, 1}};
        kir::eval_cpu_kernel(*plan.block_graph, plan.block, kb0, 2, plan.block.local_size[0], &alloc, static_cast<crd::u32>(plan.nblocks));
        kir::KernelBuffer kb1[2] = {{part64.data(), plan.nblocks, 0, 0}, {&out64, 1, 0, 1}};
        kir::eval_cpu_kernel(*plan.final_graph, plan.final_pass, kb1, 2, plan.final_pass.local_size[0], &alloc, 1U);

        // GPU: compile both entries, chain the 2 dispatches (partials round-trip via host).
        kir::GlslKernel kb(&alloc); kir::GlslKernel kf(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.block_graph, plan.block, &alloc, kb));
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.final_graph, plan.final_pass, &alloc, kf));
        const auto sb = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kb.source), "reduce_block", &alloc);
        const auto sf = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kf.source), "reduce_final", &alloc);
        REQUIRE(sb.ok); REQUIRE(sf.ok);
        auto pb = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sb.spirv.data(), sb.spirv.size()), 2, 0U);
        auto pf = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sf.spirv.data(), sf.spirv.size()), 2, 0U);
        REQUIRE(pb != nullptr); REQUIRE(pf != nullptr);

        crd::containers::Array<float> part32(&alloc); part32.resize(static_cast<crd::usize>(plan.nblocks), 0.0F);
        float                         out32 = -1234.0F;
        float*    h0[2]  = {x32.data(), part32.data()};
        int       l0[2]  = {n, plan.nblocks};
        crd::kir_test::dispatch_kernel_1wg(compute, *pb, h0, l0, 2, static_cast<crd::u32>(plan.nblocks));
        float*    h1[2]  = {part32.data(), &out32};
        int       l1[2]  = {plan.nblocks, 1};
        crd::kir_test::dispatch_kernel_1wg(compute, *pf, h1, l1, 2, 1U);

        CHECK(out32 == static_cast<float>(out64)); // bit-exact GPU == oracle
    }
}

// B-cmp reduction CRUSH bench: our 2-pass device reduce vs CUB DeviceReduce (bench/gpu-compute/cub_reduce_bench.exe). A
// reduction is MEMORY-BOUND (reads N once) ⇒ the metric is DRAM bandwidth. N=2^24 (64 MB) spills the 48 MB L2 ⇒ DRAM-bound,
// where CUB lands ~602 GB/s (~90% of 672 peak). GPU-timed (last_gpu_ms brackets both dispatches), min-of-30. Hidden ([.reduce-bench]).
TEST_CASE("B-cmp: CKIR device reduction -- GPU benchmark vs CUB DeviceReduce", "[.reduce-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    const int nl_cases[2] = {1 << 22, 1 << 24};        // 16 MB (L2) · 64 MB (DRAM-bound)
    const double cub_ms[2] = {0.01360, 0.11136};       // CUB DeviceReduce gold (per size)
    const double cub_gbps[2] = {1233.5, 602.6};
    for (int ci = 0; ci < 2; ++ci)
    {
        const int             n  = nl_cases[ci];
        const int             nb = n / (256 * 8);       // per_thread = 8 ⇒ small unroll
        kir::KGraph           g0(&alloc);
        kir::KGraph           g1(&alloc);
        kir::KGraph*          graphs[2] = {&g0, &g1};
        const kir::ReducePlan plan      = kir::build_reduce(graphs, n, kir::KOp::Add, 256, nb);

        kir::GlslKernel kb(&alloc); kir::GlslKernel kf(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.block_graph, plan.block, &alloc, kb));
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.final_graph, plan.final_pass, &alloc, kf));
        const auto sb = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kb.source), "red_b", &alloc);
        const auto sf = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kf.source), "red_f", &alloc);
        REQUIRE(sb.ok); REQUIRE(sf.ok);
        auto pb = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sb.spirv.data(), sb.spirv.size()), 2, 0U);
        auto pf = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sf.spirv.data(), sf.spirv.size()), 2, 0U);
        REQUIRE(pb != nullptr); REQUIRE(pf != nullptr);

        auto d_in   = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_part = compute.create_buffer(static_cast<crd::u64>(nb) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_out  = compute.create_buffer(sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        {
            auto  stg = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p   = static_cast<float*>(stg->map());
            for (int i = 0; i < n; ++i) { p[i] = 1.0F; } // sum = n, exact in f32 for n = 2^k
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *d_in, 0U, 0U, static_cast<crd::u64>(n) * sizeof(float));
            compute.submit_and_wait();
        }

        auto record = [&]() {
            auto&              rec  = compute.begin();
            cg::ComputeBuffer* b0[2] = {d_in.get(), d_part.get()};
            rec.dispatch(*pb, crd::containers::ConstSpan<cg::ComputeBuffer*>(b0, 2), nullptr, 0U, static_cast<crd::u32>(nb), 1U, 1U);
            rec.barrier(*d_part, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* b1[2] = {d_part.get(), d_out.get()};
            rec.dispatch(*pf, crd::containers::ConstSpan<cg::ComputeBuffer*>(b1, 2), nullptr, 0U, 1U, 1U, 1U);
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        auto rb = compute.create_buffer(sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*d_out, *rb, 0U, 0U, sizeof(float));
            compute.submit_and_wait();
        }
        const float got = *static_cast<const float*>(rb->map());
        rb->unmap();
        const double gbps = static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        WARN("[reduce-bench] N=" << n << " OURS " << best << " ms (" << gbps << " GB/s) vs CUB " << cub_ms[ci] << " ms (" << cub_gbps[ci]
                                 << " GB/s) = " << (cub_ms[ci] / best) << "x  sum=" << got << " (expect " << n << ")");
        CHECK(best < 1e29);
        CHECK(got == static_cast<float>(n));
    }
}

// B-cmp: the CKIR device-wide SCAN (ckir_scan.hpp) dispatches bit-exact on Vulkan vs the CPU oracle — the 3-pass plan
// (block scan → scan blocksums → add offsets). Inclusive + exclusive; small-int input ⇒ prefix sums are exact in f32.
TEST_CASE("B-cmp: CKIR device SCAN DISPATCHES on Vulkan == CPU oracle bit-exact", "[gpu-context][vulkan][gpu][kernel][scan]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    constexpr int n = 65536;
    crd::memory::TlsfAllocator alloc(64U << 20U);
    crd::containers::Array<crd::f64> x64(&alloc); crd::containers::Array<float> x32(&alloc);
    x64.resize(n); x32.resize(n);
    for (int i = 0; i < n; ++i) { x64[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 5); x32[static_cast<crd::usize>(i)] = static_cast<float>(x64[static_cast<crd::usize>(i)]); }

    for (int incl = 0; incl < 2; ++incl)
    {
        kir::KGraph g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc); kir::KGraph* gs[3] = {&g0, &g1, &g2};
        const kir::ScanPlan plan = kir::build_scan(gs, n, incl != 0, 256, 64);
        REQUIRE_FALSE(plan.single_pass);
        const int nb = plan.nblocks;

        // CPU oracle: 3 passes → out64.
        crd::containers::Array<crd::f64> loc64(&alloc); loc64.resize(n, 0.0);
        crd::containers::Array<crd::f64> bs64(&alloc);  bs64.resize(static_cast<crd::usize>(nb), 0.0);
        crd::containers::Array<crd::f64> of64(&alloc);  of64.resize(static_cast<crd::usize>(nb), 0.0);
        crd::containers::Array<crd::f64> out64(&alloc); out64.resize(n, 0.0);
        crd::f64 dummy = 0.0;
        kir::KernelBuffer a0[3] = {{x64.data(), n, 0, 0}, {loc64.data(), n, 0, 1}, {bs64.data(), nb, 0, 2}};
        kir::eval_cpu_kernel(*plan.block_graph, plan.block, a0, 3, plan.block.local_size[0], &alloc, static_cast<crd::u32>(nb));
        kir::KernelBuffer a1[3] = {{bs64.data(), nb, 0, 0}, {of64.data(), nb, 0, 1}, {&dummy, 1, 0, 2}};
        kir::eval_cpu_kernel(*plan.sums_graph, plan.scan_sums, a1, 3, plan.scan_sums.local_size[0], &alloc, 1U);
        kir::KernelBuffer a2[3] = {{loc64.data(), n, 0, 0}, {of64.data(), nb, 0, 1}, {out64.data(), n, 0, 2}};
        kir::eval_cpu_kernel(*plan.addoff_graph, plan.add_off, a2, 3, plan.add_off.local_size[0], &alloc, static_cast<crd::u32>(nb));

        // GPU: compile the 3 entries, chain the dispatches (buffers round-trip via host).
        kir::GlslKernel kk0(&alloc); kir::GlslKernel kk1(&alloc); kir::GlslKernel kk2(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.block_graph, plan.block, &alloc, kk0));
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.sums_graph, plan.scan_sums, &alloc, kk1));
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.addoff_graph, plan.add_off, &alloc, kk2));
        const auto s0 = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk0.source), "scan0", &alloc);
        const auto s1 = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk1.source), "scan1", &alloc);
        const auto s2 = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk2.source), "scan2", &alloc);
        REQUIRE(s0.ok); REQUIRE(s1.ok); REQUIRE(s2.ok);
        auto p0 = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(s0.spirv.data(), s0.spirv.size()), 3, 0U);
        auto p1 = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(s1.spirv.data(), s1.spirv.size()), 3, 0U);
        auto p2 = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(s2.spirv.data(), s2.spirv.size()), 3, 0U);
        REQUIRE(p0 != nullptr); REQUIRE(p1 != nullptr); REQUIRE(p2 != nullptr);

        crd::containers::Array<float> loc32(&alloc); loc32.resize(n, 0.0F);
        crd::containers::Array<float> bs32(&alloc);  bs32.resize(static_cast<crd::usize>(nb), 0.0F);
        crd::containers::Array<float> of32(&alloc);  of32.resize(static_cast<crd::usize>(nb), 0.0F);
        crd::containers::Array<float> out32(&alloc); out32.resize(n, 0.0F);
        float dm = 0.0F;
        float* hb0[3] = {x32.data(), loc32.data(), bs32.data()}; int lb0[3] = {n, n, nb};
        crd::kir_test::dispatch_kernel_1wg(compute, *p0, hb0, lb0, 3, static_cast<crd::u32>(nb));
        float* hb1[3] = {bs32.data(), of32.data(), &dm}; int lb1[3] = {nb, nb, 1};
        crd::kir_test::dispatch_kernel_1wg(compute, *p1, hb1, lb1, 3, 1U);
        float* hb2[3] = {loc32.data(), of32.data(), out32.data()}; int lb2[3] = {n, nb, n};
        crd::kir_test::dispatch_kernel_1wg(compute, *p2, hb2, lb2, 3, static_cast<crd::u32>(nb));

        int bad = 0;
        for (int i = 0; i < n; ++i) { if (out32[static_cast<crd::usize>(i)] != static_cast<float>(out64[static_cast<crd::usize>(i)])) { ++bad; } }
        CHECK(bad == 0);
    }
}

// B-cmp scan bench: our portable 3-pass device scan vs CUB DeviceScan (bench/gpu-compute/cub_scan_bench.exe). Scan is
// memory-bound; CUB is SINGLE-PASS (~2N traffic, device atomics), ours is a portable NO-ATOMICS 3-pass (~4N) ⇒ an honest
// multi-pass tax. GPU-timed (last_gpu_ms over the 3 dispatches), min-of-30. Hidden ([.scan-bench]).
TEST_CASE("B-cmp: CKIR device scan -- GPU benchmark vs CUB DeviceScan", "[.scan-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    const int    nl_cases[2] = {1 << 22, 1 << 24};
    const double cub_ms[2]   = {0.02124, 0.22709};
    const double cub_gbps[2] = {1579.4, 591.0};
    for (int ci = 0; ci < 2; ++ci)
    {
        const int             n  = nl_cases[ci];
        const int             nb = n / 4096; // elems_per_block=4096 (16 KB shared), per_thread=16
        kir::KGraph           g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc); kir::KGraph* gs[3] = {&g0, &g1, &g2};
        const kir::ScanPlan   plan = kir::build_scan(gs, n, true, 256, nb);

        kir::GlslKernel kk0(&alloc); kir::GlslKernel kk1(&alloc); kir::GlslKernel kk2(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.block_graph, plan.block, &alloc, kk0));
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.sums_graph, plan.scan_sums, &alloc, kk1));
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.addoff_graph, plan.add_off, &alloc, kk2));
        const auto s0 = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk0.source), "sc0", &alloc);
        const auto s1 = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk1.source), "sc1", &alloc);
        const auto s2 = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk2.source), "sc2", &alloc);
        REQUIRE(s0.ok); REQUIRE(s1.ok); REQUIRE(s2.ok);
        auto p0 = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(s0.spirv.data(), s0.spirv.size()), 3, 0U);
        auto p1 = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(s1.spirv.data(), s1.spirv.size()), 3, 0U);
        auto p2 = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(s2.spirv.data(), s2.spirv.size()), 3, 0U);
        REQUIRE(p0 != nullptr); REQUIRE(p1 != nullptr); REQUIRE(p2 != nullptr);

        auto d_in  = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_loc = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_bs  = compute.create_buffer(static_cast<crd::u64>(nb) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_of  = compute.create_buffer(static_cast<crd::u64>(nb) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_fin = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        {
            auto stg = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p  = static_cast<float*>(stg->map());
            for (int i = 0; i < n; ++i) { p[i] = 1.0F; }
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *d_in, 0U, 0U, static_cast<crd::u64>(n) * sizeof(float));
            compute.submit_and_wait();
        }

        auto record = [&]() {
            auto& rec = compute.begin();
            cg::ComputeBuffer* b0[3] = {d_in.get(), d_loc.get(), d_bs.get()};
            rec.dispatch(*p0, crd::containers::ConstSpan<cg::ComputeBuffer*>(b0, 3), nullptr, 0U, static_cast<crd::u32>(nb), 1U, 1U);
            rec.barrier(*d_bs, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* b1[3] = {d_bs.get(), d_of.get(), d_bs.get()};
            rec.dispatch(*p1, crd::containers::ConstSpan<cg::ComputeBuffer*>(b1, 3), nullptr, 0U, 1U, 1U, 1U);
            rec.barrier(*d_of, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* b2[3] = {d_loc.get(), d_of.get(), d_fin.get()};
            rec.dispatch(*p2, crd::containers::ConstSpan<cg::ComputeBuffer*>(b2, 3), nullptr, 0U, static_cast<crd::u32>(nb), 1U, 1U);
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        auto rb = compute.create_buffer(sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*d_fin, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*d_fin, *rb, static_cast<crd::u64>(n - 1) * sizeof(float), 0U, sizeof(float));
            compute.submit_and_wait();
        }
        const float last = *static_cast<const float*>(rb->map());
        rb->unmap();
        const double gbps = 2.0 * static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        WARN("[scan-bench] N=" << n << " OURS(3-pass) " << best << " ms (" << gbps << " GB/s@2N) vs CUB " << cub_ms[ci] << " ms ("
                               << cub_gbps[ci] << ") = " << (cub_ms[ci] / best) << "x  last=" << last << " (expect " << n << ")");
        CHECK(best < 1e29);
        CHECK(last == static_cast<float>(n));
    }
}

// ⭐⭐⭐ THE SCAN CRUSH — the SINGLE-PASS chained scan (2N traffic, matching CUB) vs CUB DeviceScan. One dispatch: each block
// scans its span then spins on the coherent flag of its predecessor, reads its published prefix, publishes its own. Our
// kernels hit ~94% peak vs CUB's ~88%, so a 2N single-pass should beat CUB's 0.227 ms. Flag buffer zeroed each run. Self-
// checks (input all-1.0 ⇒ inclusive scan last = N, out[0]=1). Hidden ([.scan-sp-bench]). ⚠ relies on GPU forward progress.
TEST_CASE("B-cmp: CKIR SINGLE-PASS scan -- the crush vs CUB DeviceScan", "[.scan-sp-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    const int    nl_cases[2] = {1 << 22, 1 << 24};
    const double cub_ms[2]   = {0.02124, 0.22709};
    for (int ci = 0; ci < 2; ++ci)
    {
        const int n   = nl_cases[ci];
        const int epb = 4096;         // elems/block (16 KB shared), pt=16
        const int nb  = n / epb;      // blocks (chain length)
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::build_scan_single_pass(g, epb, 256, true);

        kir::GlslKernel kk(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kk));
        const auto sp = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kk.source), "scan_sp", &alloc);
        if (!sp.ok) { WARN("[scan-sp] compile FAILED: " << sp.error_message.c_str()); continue; }
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sp.spirv.data(), sp.spirv.size()), 4, 0U);
        REQUIRE(pipe != nullptr);

        auto d_in  = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_out = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_agg = compute.create_buffer(static_cast<crd::u64>(nb) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_flg = compute.create_buffer(static_cast<crd::u64>(nb) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto zero  = compute.create_buffer(static_cast<crd::u64>(nb) * sizeof(crd::u32), transfer_src, cg::ComputeMemory::CpuToGpu);
        { auto* z = static_cast<crd::u32*>(zero->map()); for (int i = 0; i < nb; ++i) { z[i] = 0U; } zero->unmap(); }
        {
            auto stg = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p  = static_cast<float*>(stg->map());
            for (int i = 0; i < n; ++i) { p[i] = 1.0F; }
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *d_in, 0U, 0U, static_cast<crd::u64>(n) * sizeof(float));
            compute.submit_and_wait();
        }

        auto record = [&]() {
            auto& rec = compute.begin();
            rec.copy(*zero, *d_flg, 0U, 0U, static_cast<crd::u64>(nb) * sizeof(crd::u32)); // clear flags
            rec.barrier(*d_flg, cg::ComputeAccess::TransferDst, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* b[4] = {d_in.get(), d_out.get(), d_agg.get(), d_flg.get()};
            rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(b, 4), nullptr, 0U, static_cast<crd::u32>(nb), 1U, 1U);
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        auto rb = compute.create_buffer(2U * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*d_out, *rb, 0U, 0U, sizeof(float));                                    // out[0]
            rec.copy(*d_out, *rb, static_cast<crd::u64>(n - 1) * sizeof(float), sizeof(float), sizeof(float)); // out[n-1]
            compute.submit_and_wait();
        }
        const auto* o = static_cast<const float*>(rb->map());
        const float first = o[0]; const float last = o[1];
        rb->unmap();
        const double gbps = 2.0 * static_cast<double>(n) * sizeof(float) / (best * 1.0e6);
        WARN("[scan-sp] N=" << n << " OURS(1-pass) " << best << " ms (" << gbps << " GB/s@2N) vs CUB " << cub_ms[ci]
                            << " = " << (cub_ms[ci] / best) << "x  out[0]=" << first << " out[N-1]=" << last << " (expect 1," << n << ")");
        CHECK(best < 1e29);
        CHECK(first == 1.0F);
        CHECK(last == static_cast<float>(n));
    }
}

// B-cmp: the CKIR stable LSD radix sort DISPATCHES on Vulkan (full 4-pass histogram → offset → scatter pipeline, ping-pong) —
// the whole compute system running end-to-end portably. Output verified sorted + a valid permutation (XOR+sum). Serial-rank
// scatter for now (correctness); the parallel rank is the crush follow-on.
TEST_CASE("B-cmp: CKIR radix sort DISPATCHES on Vulkan == sorted permutation", "[gpu-context][vulkan][gpu][kernel][sort]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(64U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int n          = 16384;
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 256;
    constexpr int epb        = 1024;
    constexpr int nblocks    = n / epb;

    // compile 4 histogram + 1 offset + 4 scatter pipelines.
    constexpr int scan_threads = nblocks < threads ? nblocks : threads; // divides nblocks
    std::unique_ptr<cg::ComputePipeline> ph_s[4];
    std::unique_ptr<cg::ComputePipeline> ps_s[4];
    std::unique_ptr<cg::ComputePipeline> po1_s;
    std::unique_ptr<cg::ComputePipeline> po2_s;
    cg::ComputePipeline*                 ph[4] = {};
    cg::ComputePipeline*                 ps[4] = {};
    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V compile failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph gof1(&alloc); kir::KGraph gof2(&alloc);
    po1_s = mk(gof1, kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads), 3, "sort_off1");
    po2_s = mk(gof2, kir::build_sort_gbase(gof2, radix_bits), 2, "sort_gb");
    cg::ComputePipeline* po1 = po1_s.get();
    cg::ComputePipeline* po2 = po2_s.get();
    kir::KGraph ghg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    kir::KGraph gsg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    for (int p = 0; p < 4; ++p)
    {
        ph_s[p] = mk(ghg[p], kir::build_sort_histogram(ghg[p], epb, threads, radix_bits, p * 8, nblocks), 2, "sort_hist");
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter(gsg[p], epb, threads, radix_bits, p * 8, nblocks), 4, "sort_scat");
        ph[p] = ph_s[p].get(); ps[p] = ps_s[p].get();
        REQUIRE(ph[p] != nullptr); REQUIRE(ps[p] != nullptr);
    }
    REQUIRE(po1 != nullptr); REQUIRE(po2 != nullptr);

    crd::containers::Array<crd::u32> keys(&alloc); keys.resize(n);
    for (int i = 0; i < n; ++i) { keys[static_cast<crd::usize>(i)] = (static_cast<crd::u32>(i) * 1103515245U + 12345U) ^ (static_cast<crd::u32>(i) << 13U); }

    auto d_a  = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_b  = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_h  = compute.create_buffer(static_cast<crd::u64>(nblocks * nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_o  = compute.create_buffer(static_cast<crd::u64>(nblocks * nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_t  = compute.create_buffer(static_cast<crd::u64>(nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); // bin totals
    auto d_gb = compute.create_buffer(static_cast<crd::u64>(nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); // per-bin global base
    {
        auto stg = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p  = static_cast<crd::u32*>(stg->map());
        for (int i = 0; i < n; ++i) { p[i] = keys[static_cast<crd::usize>(i)]; }
        stg->unmap();
        auto& rec = compute.begin();
        rec.copy(*stg, *d_a, 0U, 0U, static_cast<crd::u64>(n) * sizeof(crd::u32));
        compute.submit_and_wait();
    }

    auto& rec = compute.begin();
    cg::ComputeBuffer* in = d_a.get(); cg::ComputeBuffer* out = d_b.get();
    for (int p = 0; p < 4; ++p)
    {
        cg::ComputeBuffer* hb[2] = {in, d_h.get()};
        rec.dispatch(*ph[p], crd::containers::ConstSpan<cg::ComputeBuffer*>(hb, 2), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U);
        rec.barrier(*d_h, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* o1[3] = {d_h.get(), d_o.get(), d_t.get()}; // parallel offset: local (per-bin prefix + totals)
        rec.dispatch(*po1, crd::containers::ConstSpan<cg::ComputeBuffer*>(o1, 3), nullptr, 0U, static_cast<crd::u32>(nbins), 1U, 1U);
        rec.barrier(*d_o, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        rec.barrier(*d_t, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* o2[2] = {d_t.get(), d_gb.get()}; // gbase: tiny 1-WG scan of the totals
        rec.dispatch(*po2, crd::containers::ConstSpan<cg::ComputeBuffer*>(o2, 2), nullptr, 0U, 1U, 1U, 1U);
        rec.barrier(*d_gb, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* sb[4] = {in, out, d_o.get(), d_gb.get()};
        rec.dispatch(*ps[p], crd::containers::ConstSpan<cg::ComputeBuffer*>(sb, 4), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U);
        rec.barrier(*out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* tmp = in; in = out; out = tmp;
    }
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), transfer_dst, cg::ComputeMemory::GpuToCpu);
    {
        auto& r2 = compute.begin();
        r2.barrier(*in, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        r2.copy(*in, *rb, 0U, 0U, static_cast<crd::u64>(n) * sizeof(crd::u32));
        compute.submit_and_wait();
    }
    const auto* o = static_cast<const crd::u32*>(rb->map());
    int      bad = 0;
    crd::u32 ix  = 0U;
    crd::u32 sx  = 0U;
    for (int i = 0; i < n; ++i) { if (i > 0 && o[i - 1] > o[i]) { ++bad; } ix ^= keys[static_cast<crd::usize>(i)]; sx ^= o[i]; }
    rb->unmap();
    CHECK(bad == 0);   // fully sorted
    CHECK(ix == sx);   // permutation of the input
}

// ⭐⭐⭐ THE SORT CRUSH — the full 4-pass radix sort (parallel-rank scatter) vs CUB DeviceRadixSort. Memory-bound (4 passes ×
// read+write N = ~8N traffic); at our 94% peak a memory-bound sort beats CUB's ~508 GB/s. GPU-timed over all 12 dispatches,
// min-of-N. Self-verifying (readback sorted + XOR permutation). Hidden ([.sort-bench]).
TEST_CASE("B-cmp: CKIR radix sort -- the crush vs CUB DeviceRadixSort", "[.sort-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(256U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int n          = 1 << 24; // 16.7M keys (DRAM-bound)
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 256;
    constexpr int epb        = 2048;    // 512-thread scatter: 4 rank rounds, 41KB shared (48KB Vulkan cap)
    constexpr int nblocks    = n / epb;
    const double  cub_ms     = 1.0546;

    constexpr int scan_threads = nblocks < threads ? nblocks : threads; // divides nblocks (256 here: 8192/256=32 cols/thread)
    std::unique_ptr<cg::ComputePipeline> ph_s[4];
    std::unique_ptr<cg::ComputePipeline> ps_s[4];
    std::unique_ptr<cg::ComputePipeline> po1_s;
    std::unique_ptr<cg::ComputePipeline> po2_s;
    cg::ComputePipeline*                 ph[4] = {};
    cg::ComputePipeline*                 ps[4] = {};
    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph gof1(&alloc); kir::KGraph gof2(&alloc);
    po1_s = mk(gof1, kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads), 3, "srt_off1");
    po2_s = mk(gof2, kir::build_sort_gbase(gof2, radix_bits), 2, "srt_gb");
    cg::ComputePipeline* po1 = po1_s.get();
    cg::ComputePipeline* po2 = po2_s.get();
    kir::KGraph ghg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    kir::KGraph gsg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    for (int p = 0; p < 4; ++p)
    {
        ph_s[p] = mk(ghg[p], kir::build_sort_histogram(ghg[p], epb, threads, radix_bits, p * 8, nblocks), 2, "srt_h");
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter(gsg[p], epb, threads, radix_bits, p * 8, nblocks), 4, "srt_s");
        ph[p] = ph_s[p].get(); ps[p] = ps_s[p].get();
        REQUIRE(ph[p] != nullptr); REQUIRE(ps[p] != nullptr);
    }
    REQUIRE(po1 != nullptr); REQUIRE(po2 != nullptr);

    crd::containers::Array<crd::u32> keys(&alloc); keys.resize(n);
    for (int i = 0; i < n; ++i) { keys[static_cast<crd::usize>(i)] = (static_cast<crd::u32>(i) * 2654435761U) ^ (static_cast<crd::u32>(i) << 11U); }

    auto d_a = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_b = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_h = compute.create_buffer(static_cast<crd::u64>(nblocks * nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_o = compute.create_buffer(static_cast<crd::u64>(nblocks * nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_t = compute.create_buffer(static_cast<crd::u64>(nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); // bin totals
    auto d_gb = compute.create_buffer(static_cast<crd::u64>(nbins) * sizeof(crd::u32), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); // per-bin global base
    auto stg = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), transfer_src, cg::ComputeMemory::CpuToGpu);
    { auto* p = static_cast<crd::u32*>(stg->map()); for (int i = 0; i < n; ++i) { p[i] = keys[static_cast<crd::usize>(i)]; } stg->unmap(); }

    // fresh random input each run — but in its OWN submit so the 67MB host→device copy (PCIe-bound) is NOT in last_gpu_ms.
    const auto upload = [&]() {
        auto& rc = compute.begin();
        rc.copy(*stg, *d_a, 0U, 0U, static_cast<crd::u64>(n) * sizeof(crd::u32));
        rc.barrier(*d_a, cg::ComputeAccess::TransferDst, cg::ComputeAccess::ShaderRead);
        compute.submit_and_wait();
    };
    // BATCH many back-to-back sorts into ONE submit (CUB's own bench methodology) so any fixed per-submit cost (GPU wake/clock
    // ramp/fence latency — an empty submit measures ~12 ms on this host) is amortized. Each sort re-sorts d_a in place (ping-pong
    // d_a↔d_b, even passes ⇒ ends in d_a); the last pass's barrier orders the next sort's read. GPU-timed over the whole batch.
    constexpr int batch = 32;
    const auto    record = [&]() {
        auto& rec = compute.begin();
        for (int s = 0; s < batch; ++s)
        {
            constexpr int diag = 0; // 0=full, 1=skip offset, 2=skip scatter, 3=skip histogram
            cg::ComputeBuffer* in = d_a.get(); cg::ComputeBuffer* out = d_b.get();
            for (int p = 0; p < 4; ++p)
            {
                cg::ComputeBuffer* hb[2] = {in, d_h.get()};
                if (diag != 3) { rec.dispatch(*ph[p], crd::containers::ConstSpan<cg::ComputeBuffer*>(hb, 2), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U); }
                rec.barrier(*d_h, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
                cg::ComputeBuffer* o1[3] = {d_h.get(), d_o.get(), d_t.get()};
                if (diag != 1) { rec.dispatch(*po1, crd::containers::ConstSpan<cg::ComputeBuffer*>(o1, 3), nullptr, 0U, static_cast<crd::u32>(nbins), 1U, 1U); }
                rec.barrier(*d_o, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
                rec.barrier(*d_t, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
                cg::ComputeBuffer* o2[2] = {d_t.get(), d_gb.get()};
                if (diag != 1) { rec.dispatch(*po2, crd::containers::ConstSpan<cg::ComputeBuffer*>(o2, 2), nullptr, 0U, 1U, 1U, 1U); }
                rec.barrier(*d_gb, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
                cg::ComputeBuffer* sb[4] = {in, out, d_o.get(), d_gb.get()};
                if (diag != 2) { rec.dispatch(*ps[p], crd::containers::ConstSpan<cg::ComputeBuffer*>(sb, 4), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U); }
                rec.barrier(*out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
                cg::ComputeBuffer* t = in; in = out; out = t;
            }
        }
        compute.submit_and_wait();
    };
    upload(); // once — d_a holds fresh random keys; every batched sort re-sorts the (now sorted) d_a: identical kernel work.
    for (int w = 0; w < 2; ++w) { record(); }
    double best = 1e30;      // GPU-timestamped (per sort)
    double wbest = 1e30;     // CPU wall-clock of the whole batch, per sort — robust to last_gpu_ms quirks
    for (int r = 0; r < 6; ++r)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        record();
        const auto   t1  = std::chrono::high_resolution_clock::now();
        const double wms = std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(batch);
        if (wms < wbest) { wbest = wms; }
        const double ms = compute.last_gpu_ms() / static_cast<double>(batch); if (ms > 0.0 && ms < best) { best = ms; }
    }

    auto rb = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(crd::u32), transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r2 = compute.begin(); r2.barrier(*d_a, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc); r2.copy(*d_a, *rb, 0U, 0U, static_cast<crd::u64>(n) * sizeof(crd::u32)); compute.submit_and_wait(); }
    const auto* o = static_cast<const crd::u32*>(rb->map());
    int      bad = 0;
    crd::u32 ix  = 0U;
    crd::u32 sx  = 0U;
    for (int i = 0; i < n; ++i) { if (i > 0 && o[i - 1] > o[i]) { ++bad; } ix ^= keys[static_cast<crd::usize>(i)]; sx ^= o[i]; }
    rb->unmap();
    const double mks = static_cast<double>(n) / (wbest * 1.0e3);
    WARN("[sort-bench] N=" << n << " OURS wall " << wbest << " ms (" << mks << " Mkeys/s) vs CUB " << cub_ms << " = " << (cub_ms / wbest)
                           << "x  [gpu-ts " << best << " ms]  sorted=" << (bad == 0) << " permutation=" << (ix == sx));
    CHECK(wbest < 1e29);
    CHECK(bad == 0);
    CHECK(ix == sx);
}

// B-cmp: CKIR SUBGROUP ops on Vulkan — oracle-vs-GPU bit-exactness of subgroup_ballot + exclusive-count (the radix-rank / warp-scan
// building block). out[t] = # lanes below t in its 32-subgroup with an ODD input. On this device (32-lane subgroups, linear
// local↔lane mapping) the GPU must MATCH the CPU oracle exactly. The grouping-independent composition (block rank) is the sort's job.
TEST_CASE("B-cmp: CKIR subgroup ballot+count DISPATCHES on Vulkan == oracle", "[gpu-context][vulkan][gpu][kernel][subgroup]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(8U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int threads = 64; // two 32-lane subgroups

    kir::KGraph g(&alloc);
    const auto  ku      = [&](crd::u32 v) { return g.constant(static_cast<crd::f64>(v), kir::make_shape({1}), kir::DType::U32); };
    const int   in_buf  = g.buffer_decl(kir::DType::U32, 0, 0, false);
    const int   out_buf = g.buffer_decl(kir::DType::U32, 0, 1, true);
    const int   tid     = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const int   mark    = g.kernel_stmt_mark();
    const int   rank    = g.subgroup_ballot_excl_count(g.subgroup_ballot(g.binary(kir::KOp::BitAnd, g.buffer_load(in_buf, tid), ku(1))));
    g.stmt_buffer_store(out_buf, tid, rank);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(threads);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    kir::GlslKernel k(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), "sg", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<crd::u32> host(&alloc);
    host.resize(threads);
    for (int i = 0; i < threads; ++i) { host[static_cast<crd::usize>(i)] = static_cast<crd::u32>(i * 2654435761U); }

    auto d_in  = compute.create_buffer(threads * sizeof(crd::u32), storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out = compute.create_buffer(threads * sizeof(crd::u32), storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto stg   = compute.create_buffer(threads * sizeof(crd::u32), transfer_src, cg::ComputeMemory::CpuToGpu);
    { auto* p = static_cast<crd::u32*>(stg->map()); for (int i = 0; i < threads; ++i) { p[i] = host[static_cast<crd::usize>(i)]; } stg->unmap(); }
    { auto& r = compute.begin(); r.copy(*stg, *d_in, 0U, 0U, threads * sizeof(crd::u32)); r.barrier(*d_in, cg::ComputeAccess::TransferDst, cg::ComputeAccess::ShaderRead);
      cg::ComputeBuffer* b[2] = {d_in.get(), d_out.get()};
      r.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(b, 2), nullptr, 0U, 1U, 1U, 1U);
      r.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc); compute.submit_and_wait(); }
    auto rb = compute.create_buffer(threads * sizeof(crd::u32), transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r = compute.begin(); r.copy(*d_out, *rb, 0U, 0U, threads * sizeof(crd::u32)); compute.submit_and_wait(); }
    const auto* got = static_cast<const crd::u32*>(rb->map());

    int bad = 0;
    for (int t = 0; t < threads; ++t)
    {
        const int sgbase = (t / 32) * 32;
        crd::u32  ref    = 0U;
        for (int l = sgbase; l < t; ++l) { if ((host[static_cast<crd::usize>(l)] & 1U) != 0U) { ++ref; } }
        if (got[t] != ref) { ++bad; }
    }
    rb->unmap();
    CHECK(bad == 0); // GPU subgroup ballot/count == CPU oracle, bit-exact
}

// B-cmp: STANDALONE per-kernel sort profiler — each kernel batch-timed ALONE with VALID precomputed inputs (the skip-diag
// method is contaminated: skipping a kernel feeds stale data downstream and changes the others' timing; feeding each kernel
// real inputs and timing it solo is the correct isolation). Pass-0 configuration, random keys. Hidden ([.sort-kprof]).
TEST_CASE("B-cmp: radix sort PER-KERNEL standalone profile", "[.sort-kprof]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(256U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int n            = 1 << 24;
    constexpr int threads      = 256;
    constexpr int radix_bits   = 8;
    constexpr int nbins        = 256;
    constexpr int epb          = 2048;
    constexpr int nblocks      = n / epb;
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph gh(&alloc); kir::KGraph go1(&alloc); kir::KGraph go2(&alloc); kir::KGraph gs(&alloc);
    auto ph  = mk(gh, kir::build_sort_histogram(gh, epb, threads, radix_bits, 0, nblocks), 2, "kp_h");
    auto po1 = mk(go1, kir::build_sort_offset_local(go1, nblocks, radix_bits, scan_threads), 3, "kp_o1");
    auto po2 = mk(go2, kir::build_sort_gbase(go2, radix_bits), 2, "kp_gb");
    auto ps  = mk(gs, kir::build_sort_scatter(gs, epb, threads, radix_bits, 0, nblocks), 4, "kp_s");
    REQUIRE(ph != nullptr); REQUIRE(po1 != nullptr); REQUIRE(po2 != nullptr); REQUIRE(ps != nullptr);

    auto d_a = compute.create_buffer(static_cast<crd::u64>(n) * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_b = compute.create_buffer(static_cast<crd::u64>(n) * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_h = compute.create_buffer(static_cast<crd::u64>(nblocks * nbins) * 4U, storage, cg::ComputeMemory::GpuOnly);
    auto d_o = compute.create_buffer(static_cast<crd::u64>(nblocks * nbins) * 4U, storage, cg::ComputeMemory::GpuOnly);
    auto d_t = compute.create_buffer(static_cast<crd::u64>(nbins) * 4U, storage, cg::ComputeMemory::GpuOnly);
    auto d_gb = compute.create_buffer(static_cast<crd::u64>(nbins) * 4U, storage, cg::ComputeMemory::GpuOnly);
    {
        auto stg = compute.create_buffer(static_cast<crd::u64>(n) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p  = static_cast<crd::u32*>(stg->map());
        for (int i = 0; i < n; ++i) { p[i] = (static_cast<crd::u32>(i) * 2654435761U) ^ (static_cast<crd::u32>(i) << 11U); }
        stg->unmap();
        auto& r = compute.begin();
        r.copy(*stg, *d_a, 0U, 0U, static_cast<crd::u64>(n) * 4U);
        compute.submit_and_wait();
    }

    // one real pass to produce VALID d_h, d_o, d_t for the standalone runs.
    const auto prep = [&]() {
        auto& r = compute.begin();
        cg::ComputeBuffer* hb[2] = {d_a.get(), d_h.get()};
        r.dispatch(*ph, crd::containers::ConstSpan<cg::ComputeBuffer*>(hb, 2), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U);
        r.barrier(*d_h, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* o1[3] = {d_h.get(), d_o.get(), d_t.get()};
        r.dispatch(*po1, crd::containers::ConstSpan<cg::ComputeBuffer*>(o1, 3), nullptr, 0U, static_cast<crd::u32>(nbins), 1U, 1U);
        r.barrier(*d_o, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        r.barrier(*d_t, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* o2[2] = {d_t.get(), d_gb.get()};
        r.dispatch(*po2, crd::containers::ConstSpan<cg::ComputeBuffer*>(o2, 2), nullptr, 0U, 1U, 1U, 1U);
        r.barrier(*d_gb, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        compute.submit_and_wait();
    };
    prep();

    constexpr int  kbatch = 64;
    const auto solo = [&](const char* nm, cg::ComputePipeline& pipe, cg::ComputeBuffer** bufs, int nb, cg::ComputeBuffer& outb, crd::u32 grid) -> double {
        const auto run = [&]() {
            auto& r = compute.begin();
            for (int it = 0; it < kbatch; ++it)
            {
                r.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(bufs, nb), nullptr, 0U, grid, 1U, 1U);
                r.barrier(outb, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            }
            compute.submit_and_wait();
        };
        run(); // warm
        double best = 1e30;
        for (int rep = 0; rep < 5; ++rep)
        {
            run();
            const double ms = compute.last_gpu_ms() / static_cast<double>(kbatch);
            if (ms > 0.0 && ms < best) { best = ms; }
        }
        WARN("[kprof] " << nm << "  " << best << " ms");
        return best;
    };

    cg::ComputeBuffer* hb[2] = {d_a.get(), d_h.get()};
    cg::ComputeBuffer* o1[3] = {d_h.get(), d_o.get(), d_t.get()};
    cg::ComputeBuffer* sb[4] = {d_a.get(), d_b.get(), d_o.get(), d_gb.get()};
    cg::ComputeBuffer* o2[2] = {d_t.get(), d_gb.get()};
    const double th  = solo("histogram    ", *ph, hb, 2, *d_h, static_cast<crd::u32>(nblocks));
    const double to1 = solo("offset_local ", *po1, o1, 3, *d_o, static_cast<crd::u32>(nbins));
    const double ts  = solo("scatter      ", *ps, sb, 4, *d_b, static_cast<crd::u32>(nblocks));
    const double to2 = solo("gbase (1 WG) ", *po2, o2, 2, *d_gb, 1U);
    WARN("[kprof] SUM x4 passes = " << 4.0 * (th + to1 + to2 + ts) << " ms  (hist " << 4 * th << " + off_l " << 4 * to1
                                    << " + gb " << 4 * to2 << " + scat " << 4 * ts << ")");
    CHECK(th > 0.0); CHECK(to1 > 0.0); CHECK(to2 > 0.0); CHECK(ts > 0.0);
}

// *** B-cmp ONESWEEP radix sort — integer decoupled-lookback (bit-exact: u32 count sums are order-independent, so the
// timing-dependent lookback arrival order yields IDENTICAL bytes; the f32 scan wall does NOT apply to sort). Per sort:
// clear aux → fused 4-digit global histogram (ONE N-read) → gbase4 (grid=4) → 4 lookback scatters. Correctness: sorted +
// permutation — for a keys-only u32 sort that pins the output bytes UNIQUELY (equal keys are indistinguishable), so
// GPU == oracle bit-exactness holds by construction. Hidden bench ([.sort-osw]).
TEST_CASE("B-cmp: ONESWEEP radix sort -- lookback, the crush structure", "[.sort-osw]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(256U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int n          = 1 << 24;
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 256;
    constexpr int epb        = 2048;
    constexpr int nblocks    = n / epb;
    constexpr int aux_words  = 4 * nbins + 4 + 4 * nblocks * nbins; // [ghist | 4 tickets | look]
    constexpr int clear_grid = (aux_words + threads * 8 - 1) / (threads * 8);
    const double  cub_ms     = 1.0546;

    const auto mk = [&](kir::KGraph& g, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
        kir::GlslKernel k(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, k));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k.source), nm, &alloc);
        if (!spv.ok) { WARN("[" << nm << "] SPIR-V failed: " << spv.error_message.c_str()); }
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    kir::KGraph gcl(&alloc); kir::KGraph ggh(&alloc); kir::KGraph ggb(&alloc);
    auto pcl = mk(gcl, kir::build_sort_clear(gcl, aux_words, threads), 1, "osw_clr");
    auto pgh = mk(ggh, kir::build_sort_ghist(ggh, epb, threads, radix_bits), 2, "osw_gh");
    auto pgb = mk(ggb, kir::build_sort_gbase(ggb, radix_bits), 2, "osw_gb");
    std::unique_ptr<cg::ComputePipeline> ps_s[4];
    cg::ComputePipeline*                 ps[4] = {};
    kir::KGraph gsg[4] = {kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc), kir::KGraph(&alloc)};
    for (int p = 0; p < 4; ++p)
    {
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter_onesweep(gsg[p], epb, threads, radix_bits, p * 8, p, nblocks), 4, "osw_s");
        ps[p]   = ps_s[p].get();
        REQUIRE(ps[p] != nullptr);
    }
    REQUIRE(pcl != nullptr); REQUIRE(pgh != nullptr); REQUIRE(pgb != nullptr);

    crd::containers::Array<crd::u32> keys(&alloc); keys.resize(n);
    for (int i = 0; i < n; ++i) { keys[static_cast<crd::usize>(i)] = (static_cast<crd::u32>(i) * 2654435761U) ^ (static_cast<crd::u32>(i) << 11U); }

    auto d_a  = compute.create_buffer(static_cast<crd::u64>(n) * 4U, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_b  = compute.create_buffer(static_cast<crd::u64>(n) * 4U, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_gb = compute.create_buffer(static_cast<crd::u64>(4 * nbins) * 4U, storage, cg::ComputeMemory::GpuOnly);
    auto d_ax = compute.create_buffer(static_cast<crd::u64>(aux_words) * 4U, storage, cg::ComputeMemory::GpuOnly);
    auto stg  = compute.create_buffer(static_cast<crd::u64>(n) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
    { auto* p = static_cast<crd::u32*>(stg->map()); for (int i = 0; i < n; ++i) { p[i] = keys[static_cast<crd::usize>(i)]; } stg->unmap(); }
    { auto& r = compute.begin(); r.copy(*stg, *d_a, 0U, 0U, static_cast<crd::u64>(n) * 4U); r.barrier(*d_a, cg::ComputeAccess::TransferDst, cg::ComputeAccess::ShaderRead); compute.submit_and_wait(); }

    constexpr int batch  = 32;
    const auto    record = [&]() {
        auto& rec = compute.begin();
        for (int s = 0; s < batch; ++s)
        {
            cg::ComputeBuffer* cb[1] = {d_ax.get()};
            rec.dispatch(*pcl, crd::containers::ConstSpan<cg::ComputeBuffer*>(cb, 1), nullptr, 0U, static_cast<crd::u32>(clear_grid), 1U, 1U);
            rec.barrier(*d_ax, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* hb[2] = {d_a.get(), d_ax.get()};
            rec.dispatch(*pgh, crd::containers::ConstSpan<cg::ComputeBuffer*>(hb, 2), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U);
            rec.barrier(*d_ax, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* gbv[2] = {d_ax.get(), d_gb.get()};
            rec.dispatch(*pgb, crd::containers::ConstSpan<cg::ComputeBuffer*>(gbv, 2), nullptr, 0U, 4U, 1U, 1U);
            rec.barrier(*d_gb, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* in = d_a.get(); cg::ComputeBuffer* out = d_b.get();
            for (int p = 0; p < 4; ++p)
            {
                cg::ComputeBuffer* sb[4] = {in, out, d_gb.get(), d_ax.get()};
                rec.dispatch(*ps[p], crd::containers::ConstSpan<cg::ComputeBuffer*>(sb, 4), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U);
                rec.barrier(*out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
                cg::ComputeBuffer* t = in; in = out; out = t;
            }
        }
        compute.submit_and_wait();
    };
    for (int w = 0; w < 2; ++w) { record(); }
    double best = 1e30; double wbest = 1e30;
    for (int r = 0; r < 6; ++r)
    {
        const auto t0 = std::chrono::high_resolution_clock::now();
        record();
        const auto   t1  = std::chrono::high_resolution_clock::now();
        const double wms = std::chrono::duration<double, std::milli>(t1 - t0).count() / static_cast<double>(batch);
        if (wms < wbest) { wbest = wms; }
        const double ms = compute.last_gpu_ms() / static_cast<double>(batch); if (ms > 0.0 && ms < best) { best = ms; }
    }

    auto rb = compute.create_buffer(static_cast<crd::u64>(n) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r2 = compute.begin(); r2.barrier(*d_a, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc); r2.copy(*d_a, *rb, 0U, 0U, static_cast<crd::u64>(n) * 4U); compute.submit_and_wait(); }
    const auto* o = static_cast<const crd::u32*>(rb->map());
    int      bad = 0;
    crd::u32 ix  = 0U;
    crd::u32 sx  = 0U;
    for (int i = 0; i < n; ++i) { if (i > 0 && o[i - 1] > o[i]) { ++bad; } ix ^= keys[static_cast<crd::usize>(i)]; sx ^= o[i]; }
    rb->unmap();
    const double mks = static_cast<double>(n) / (wbest * 1.0e3);
    WARN("[sort-osw] N=" << n << " ONESWEEP wall " << wbest << " ms (" << mks << " Mkeys/s) vs CUB " << cub_ms << " = "
                         << (cub_ms / wbest) << "x  [gpu-ts " << best << " ms]  sorted=" << (bad == 0) << " permutation=" << (ix == sx));
    CHECK(wbest < 1e29);
    CHECK(bad == 0);
    CHECK(ix == sx);
}
