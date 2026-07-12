// test_vulkan_context.cpp — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own,
// with a compute queue + (on capable adapters) the coopmat2 tensor feature — no rendering RHI, no swapchain. This is
// the foundation kir-vulkan migrates onto in v17-i-b.

#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/kir/ckir.hpp>      // C1-c: create_program(KGraph, KEntry) — the IR on-ramp
#include <crd/kir/ckir_hlsl.hpp> // B3-d: emit_stage_hlsl (the HLSL VS/FS emitter)

#include <ckir_raster_triangle.hpp> // B3-e: the SHARED, backend-neutral CKIR triangle (identical on Vulkan + DX12)

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("D-007 B8-m: THE CULMINATION — skinned + textured + lit + IBL + PCF-shadowed master material on Vulkan",
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

TEST_CASE("D-007 B12: IR screen-space lighting frontier (AO/SSILVB · SSR · SSGI · volumetrics · SSS) renders on Vulkan",
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
