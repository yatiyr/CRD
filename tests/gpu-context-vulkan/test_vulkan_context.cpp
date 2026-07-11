// test_vulkan_context.cpp — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own,
// with a compute queue + (on capable adapters) the coopmat2 tensor feature — no rendering RHI, no swapchain. This is
// the foundation kir-vulkan migrates onto in v17-i-b.

#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/kir/ckir.hpp> // C1-c: create_program(KGraph, KEntry) — the IR on-ramp

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

    // A RASTER entry has no stage emitter yet (D-007 B3-c) — refused loudly, never guessed.
    kir::KEntry vs;
    vs.stage       = kir::KStage::Vertex;
    vs.n_out       = 1;
    vs.out[0].node = out;
    CHECK(ctx->create_program(g, vs) == nullptr);
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
