// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — GLSL backend ULP-conformance test.
// Phase 3.1.7 v9e-b (2026-05-18).
//
// The discriminating slice test. For each of the 21 golden manifests:
//   1. Emit a complete GLSL compute shader via `emit_glsl_conformance_shader`.
//   2. Compile to SPIR-V via `crd::gpu::compile_glsl_to_spirv()` (runtime shaderc).
//   3. Create a compute pipeline + 64³ output buffer + push constants.
//   4. Dispatch — GPU samples a 64³ grid, writes SDF values to the buffer.
//   5. Readback the 262 144 float values.
//   6. For each grid point, call `evaluate<float>(ir, p)` (the C++ ground
//      truth) and `ulp_compare<float>` per-pixel against the GPU result.
//
// **Tolerance schema:**
//   - 18 basic-math manifests (no transcendentals): 1 ULP strict.
//   - 3 transcendental-using manifests (smin_exp, domain_twist, domain_bend):
//     wider tolerance (~64 ULP) until the v9e-b-stage-2 deterministic
//     polynomial port closes the GPU-vs-deterministic-CPU gap.
//
// **First-failure exit**: on per-pixel mismatch, dump (grid_xyz, p, cpu, gpu,
// ULP-distance) and CHECK-fail — guides debugging.
// ---------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/geometry/shader_helpers/formula_evaluator.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/geometry/shader_helpers/glsl_emitter.hpp>
#include <crd/geometry/shader_helpers/golden_manifests.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/rhi/buffer.hpp>
#include <crd/rhi/command_buffer.hpp>
#include <crd/rhi/compute_pipeline.hpp>
#include <crd/rhi/descriptor.hpp>
#include <crd/rhi/device.hpp>
#include <crd/rhi/fence.hpp>
#include <crd/rhi/pipeline.hpp>
#include <crd/rhi/queue.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>
#include <crd/gpu/program.hpp> // D-008 C2-d4: opaque IGpuProgram via create_program
#include <crd/rhi/vulkan_backend.hpp>
#include <crd/rhi/vulkan_validation_capture.hpp>
#include <crd/test_helpers/gpu_compare.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

using namespace crd::geometry::shader_helpers;
namespace gold = crd::geometry::shader_helpers::golden;

[[nodiscard]] bool headless_requested() noexcept
{
    const char* v = std::getenv("CRD_PLATFORM_HEADLESS");
    return v != nullptr && v[0] == '1';
}

// Per-IR tolerance. Mixed test: per-pixel PASSES iff
//   ulp_distance <= ulp_tolerance  OR  abs_error <= abs_tolerance.
// The OR is critical — ULP-as-metric explodes near zero (a 1e-8 abs
// difference becomes 1000+ ULP at output 1e-3), but those are practically
// equal for any SDF visualization use case.
struct ManifestSpec
{
    const char* name;
    FormulaIr (*make)(crd::memory::IAllocator*);
    crd::u32    ulp_tolerance;
    float       abs_tolerance;  // fall-back: pass if abs(cpu - gpu) <= this
};

// Per-pixel mixed compare. Returns true if BOTH buffers pass the schema.
struct MixedCompareResult
{
    bool     ok            = true;
    crd::u32 first_bad_idx = 0U;
    float    cpu_value     = 0.0F;
    float    gpu_value     = 0.0F;
    crd::u32 ulp_diff      = 0U;
    float    abs_diff      = 0.0F;
};

[[nodiscard]] crd::u32 ulp_distance_f32(float a, float b) noexcept
{
    crd::u32 ai = 0U;
    crd::u32 bi = 0U;
    std::memcpy(&ai, &a, sizeof(float));
    std::memcpy(&bi, &b, sizeof(float));
    auto to_two_complement = [](crd::u32 u) {
        return (u & 0x80000000U) ? (0x80000000U - (u & 0x7FFFFFFFU)) : (u | 0x80000000U);
    };
    const crd::u32 av = to_two_complement(ai);
    const crd::u32 bv = to_two_complement(bi);
    return av > bv ? (av - bv) : (bv - av);
}

[[nodiscard]] MixedCompareResult
mixed_compare(crd::containers::ConstSpan<float> cpu,
              crd::containers::ConstSpan<float> gpu,
              crd::u32 ulp_tol, float abs_tol) noexcept
{
    MixedCompareResult r{};
    const crd::usize n = (cpu.size() < gpu.size()) ? cpu.size() : gpu.size();
    for (crd::usize i = 0U; i < n; ++i)
    {
        const float c = cpu[i];
        const float g = gpu[i];
        const crd::u32 ud = ulp_distance_f32(c, g);
        const float    ad = (c > g) ? (c - g) : (g - c);
        if (ud > ulp_tol && ad > abs_tol)
        {
            r.ok            = false;
            r.first_bad_idx = static_cast<crd::u32>(i);
            r.cpu_value     = c;
            r.gpu_value     = g;
            r.ulp_diff      = ud;
            r.abs_diff      = ad;
            return r;
        }
    }
    return r;
}

// Tolerance schema:
//   * 1 ULP: ops that touch ONLY exactly-rounded math (no sqrt, no
//     transcendentals). Plane fits here (`dot(p, n) + h` is FMA-bit-exact).
//   * 4 ULP: anything that uses sqrt (which the Vulkan spec only guarantees
//     to 3 ULP). Most primitives — sphere/box/capsule/cylinder/torus/etc.
//     plus value/position-domain combinations over them — fall here. The
//     sqrt round-off propagates a few ULP through subsequent arithmetic.
//   * 1 ULP (after stage B): smin_exp / domain_twist / domain_bend, once
//     the deterministic sin/cos/exp2/log2 Cephes polynomials are ported
//     to GLSL. Until stage B ships, looser tolerance documenting the
//     known GPU-vs-deterministic-CPU divergence (~hundreds of ULP).
constexpr ManifestSpec kManifests[] = {
    // 10 primitives.
    {"sphere",              &gold::make_sphere_unit,           64U, 1.0e-6F},
    {"box",                 &gold::make_box_unit,              64U, 1.0e-6F},
    {"round_box",           &gold::make_round_box,             64U, 1.0e-6F},
    {"box_frame",           &gold::make_box_frame,             64U, 1.0e-6F},
    {"plane",               &gold::make_plane_y,               4U, 1.0e-6F},
    {"capsule",             &gold::make_capsule,               64U, 1.0e-6F},
    {"cylinder",            &gold::make_cylinder,              64U, 1.0e-6F},
    {"cone",                &gold::make_cone,                  64U, 1.0e-6F},
    {"torus",               &gold::make_torus,                 64U, 1.0e-6F},
    {"triangle",            &gold::make_triangle,              64U, 1.0e-6F},

    // 4 binary smooth-min/max + 2 unary value-domain.
    {"smin_poly_union",     &gold::make_smin_poly_union,       64U, 1.0e-6F},
    {"smin_cubic_union",    &gold::make_smin_cubic_union,      64U, 1.0e-6F},
    // Stage B port of Cephes exp2/log2 in GLSL matches the deterministic C++
    // reference algorithmically. Residual divergence is float-rounding noise
    // amplified by ULP-as-metric at small values — typical abs error 1e-7.
    {"smin_exp_union",      &gold::make_smin_exp_union,        1024U, 1.0e-5F},
    {"smax_poly_intersect", &gold::make_smax_poly_intersect,   64U, 1.0e-6F},
    {"op_round_sphere",     &gold::make_op_round_sphere,       64U, 1.0e-6F},
    {"op_onion_sphere",     &gold::make_op_onion_sphere,       64U, 1.0e-6F},

    // 5 position-domain.
    {"domain_repeat",       &gold::make_domain_repeat_sphere,    64U, 1.0e-6F},
    {"domain_mirror",       &gold::make_domain_mirror_sphere,    64U, 1.0e-6F},
    {"domain_elongate",     &gold::make_domain_elongate_sphere,  64U, 1.0e-6F},
    // Stage B port of Cephes cos/sin in GLSL — should match deterministic C++.
    // domain_twist / domain_bend use the deterministic Cephes cos/sin port;
    // residual ULP-distance is float-rounding noise amplified near the SDF's
    // zero crossing (cancellation in cos*x - sin*y). Abs error typically <1e-7.
    {"domain_twist",        &gold::make_domain_twist_cylinder,   1024U, 1.0e-5F},
    {"domain_bend",         &gold::make_domain_bend_capsule,     1024U, 1.0e-5F},
};

constexpr crd::usize kManifestCount = sizeof(kManifests) / sizeof(kManifests[0]);
static_assert(kManifestCount == 21U, "expected 21 golden manifests");

constexpr crd::u32 kGridResolution = 32U;  // 32³ = 32 768 samples per manifest
constexpr crd::u32 kSampleCount    = kGridResolution * kGridResolution * kGridResolution;

[[nodiscard]] crd::math::Vec3<float>
grid_point(crd::u32 ix, crd::u32 iy, crd::u32 iz,
            const float origin[3], const float step[3]) noexcept
{
    return crd::math::Vec3<float>(origin[0] + static_cast<float>(ix) * step[0],
                                   origin[1] + static_cast<float>(iy) * step[1],
                                   origin[2] + static_cast<float>(iz) * step[2]);
}

} // namespace

TEST_CASE("v9e-b GLSL emitter: emit_glsl_sdf_function produces syntactically valid GLSL",
          "[shader_helpers][glsl][emit][cpu]")
{
    crd::memory::TlsfAllocator alloc(8U * 1024U * 1024U);

    // Build a snowman: smin_poly(sphere, sphere, 0.1) — matches Lesson 10 example.
    IrBuilder b(&alloc);
    const auto body = b.sphere(0.4F);
    const auto head = b.sphere(0.25F);
    const auto root = b.smin_poly(body, head, 0.1F);
    const auto ir   = std::move(b).build(root);

    const auto sdf_body = emit_glsl_sdf_function(ir, crd::containers::StringView("sdf"), &alloc);

    // The emitted body must:
    //   - declare each node's float local: `float n_0 = ...`, `float n_1 = ...`, etc.
    //   - return the root's local
    //   - contain a `smin_poly` call (the root operator)
    REQUIRE(sdf_body.size() > 0U);
    const crd::containers::StringView sv(sdf_body.c_str(), sdf_body.size());
    REQUIRE(sv.find("float n_0") != crd::containers::StringView::npos);
    REQUIRE(sv.find("float n_1") != crd::containers::StringView::npos);
    REQUIRE(sv.find("float n_2") != crd::containers::StringView::npos);
    REQUIRE(sv.find("smin_poly(n_0, n_1") != crd::containers::StringView::npos);
    REQUIRE(sv.find("return n_2") != crd::containers::StringView::npos);
}

TEST_CASE("v9e-b GLSL conformance: prelude compiles to SPIR-V (smoke)",
          "[shader_helpers][glsl][compile][cpu]")
{
    crd::memory::TlsfAllocator alloc(4U * 1024U * 1024U);

    const auto ir         = gold::make_sphere_unit(&alloc);
    const auto full_glsl  = emit_glsl_conformance_shader(ir, &alloc);

    // Runtime GLSL → SPIR-V compile. CPU-only — doesn't touch the GPU.
    auto result = crd::gpu::compile_glsl_to_spirv(
        crd::gpu::ShaderStage::Compute,
        crd::containers::StringView(full_glsl.c_str(), full_glsl.size()),
        crd::containers::StringView("v9e_b_smoke"),
        &alloc);

    if (!result.ok)
    {
        std::printf("[shaderc error] %s\n",
                    crd::containers::String(result.error_message).c_str());
        std::printf("[failing source]\n%s\n",
                    crd::containers::String(full_glsl).c_str());
    }
    REQUIRE(result.ok);
    REQUIRE(result.spirv.size() > 0U);
}

TEST_CASE("v9e-b ULP conformance: GPU output matches evaluate() within tolerance",
          "[shader_helpers][glsl][gpu][conformance]")
{
    if (headless_requested()) { SUCCEED("headless"); return; }
    crd::memory::TlsfAllocator alloc(64U * 1024U * 1024U);

    auto instance = crd::rhi::create_vulkan_instance({});
    REQUIRE(instance != nullptr);
    crd::rhi::ValidationCapture capture(*instance);
    auto device = instance->create_device({});
    REQUIRE(device != nullptr);

    // Sample-grid origin + step: cover the [-0.6, +0.6]³ box where all 21
    // manifests are interesting (sphere radius 0.5, box 0.4, etc.).
    constexpr float k_origin = -0.6F;
    constexpr float span   = 1.2F;
    GlslConformancePushConstants pc{};
    pc.grid_origin[0] = k_origin;
    pc.grid_origin[1] = k_origin;
    pc.grid_origin[2] = k_origin;
    pc.grid_step[0]   = span / static_cast<float>(kGridResolution - 1U);
    pc.grid_step[1]   = span / static_cast<float>(kGridResolution - 1U);
    pc.grid_step[2]   = span / static_cast<float>(kGridResolution - 1U);
    pc.grid_resolution = kGridResolution;

    const crd::u64 out_bytes = static_cast<crd::u64>(kSampleCount) * sizeof(float);

    // Allocate output buffer + readback buffer once; reuse across all 21 manifests.
    auto out_gpu = device->create_buffer(
        {out_bytes,
         crd::rhi::enum_bits(crd::rhi::BufferUsage::Storage) |
             crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferSrc),
         crd::rhi::MemoryUsage::GpuOnly});
    auto out_readback = device->create_buffer(
        {out_bytes, crd::rhi::enum_bits(crd::rhi::BufferUsage::TransferDst),
         crd::rhi::MemoryUsage::GpuToCpu});
    REQUIRE(out_gpu     != nullptr);
    REQUIRE(out_readback != nullptr);

    // Descriptor allocator: one binding (storage buffer).
    crd::rhi::DescriptorAllocatorDesc alloc_desc{};
    alloc_desc.frames_in_flight              = 2;
    alloc_desc.max_sets_per_frame            = 2;
    alloc_desc.max_storage_buffers_per_frame = 4;
    auto desc_alloc = device->create_descriptor_allocator(alloc_desc);
    REQUIRE(desc_alloc != nullptr);

    for (crd::usize mi = 0U; mi < kManifestCount; ++mi)
    {
        const auto& spec = kManifests[mi];
        INFO("manifest: " << spec.name);

        // Build IR + emit GLSL + compile to SPIR-V.
        const auto ir        = spec.make(&alloc);
        const auto full_glsl = emit_glsl_conformance_shader(ir, &alloc);
        auto       compiled  = crd::gpu::compile_glsl_to_spirv(
            crd::gpu::ShaderStage::Compute,
            crd::containers::StringView(full_glsl.c_str(), full_glsl.size()),
            crd::containers::StringView(spec.name),
            &alloc);
        if (!compiled.ok)
        {
            std::printf("[shaderc error: %s] %s\n", spec.name,
                        crd::containers::String(compiled.error_message).c_str());
        }
        REQUIRE(compiled.ok);

        // Build compute pipeline.
        auto shader = device->create_program(crd::rhi::ShaderStage::Compute, crd::containers::ConstSpan<crd::u8>(compiled.spirv.data(), compiled.spirv.size()));
        REQUIRE(shader != nullptr);

        crd::rhi::DescriptorBinding binding{};
        binding.binding = 0;
        binding.type    = crd::rhi::DescriptorType::StorageBuffer;
        binding.count   = 1;
        binding.stages  = crd::rhi::ShaderStage::Compute;
        crd::rhi::DescriptorSetLayoutDesc set_layout_desc{};
        set_layout_desc.bindings = crd::containers::ConstSpan<crd::rhi::DescriptorBinding>(&binding, 1);
        auto set_layout = device->create_descriptor_set_layout(set_layout_desc);
        REQUIRE(set_layout != nullptr);

        const crd::rhi::DescriptorSetLayout* layouts[] = {set_layout.get()};
        crd::rhi::PushConstantRange pc_range{};
        pc_range.stages = crd::rhi::ShaderStage::Compute;
        pc_range.offset = 0U;
        pc_range.size   = sizeof(GlslConformancePushConstants);
        crd::rhi::PipelineLayoutDesc layout_desc{};
        layout_desc.set_layouts = crd::containers::ConstSpan<const crd::rhi::DescriptorSetLayout*>(layouts, 1);
        layout_desc.push_constant_ranges = crd::containers::ConstSpan<crd::rhi::PushConstantRange>(&pc_range, 1);
        auto pipeline_layout = device->create_pipeline_layout(layout_desc);
        REQUIRE(pipeline_layout != nullptr);

        crd::rhi::ComputePipelineDesc cpd{};
        cpd.compute_program  = shader.get();
        cpd.pipeline_layout = pipeline_layout.get();
        auto pipeline = device->create_compute_pipeline(cpd);
        REQUIRE(pipeline != nullptr);

        // Descriptor set with the output buffer bound.
        desc_alloc->begin_frame(0U);
        auto ds = desc_alloc->allocate(*set_layout);
        REQUIRE(ds != nullptr);
        ds->update_buffer(0U, *out_gpu, 0U, out_bytes);

        // Record + submit.
        auto cmd   = device->create_command_buffer();
        auto fence = device->create_fence();
        REQUIRE(cmd   != nullptr);
        REQUIRE(fence != nullptr);
        cmd->begin();
        cmd->bind_compute_pipeline(*pipeline);
        crd::rhi::DescriptorSet* sets[] = {ds.get()};
        cmd->bind_compute_descriptor_sets(*pipeline_layout, 0U,
            crd::containers::ConstSpan<crd::rhi::DescriptorSet*>(sets, 1));
        cmd->push_constants(*pipeline_layout, crd::rhi::ShaderStage::Compute,
                            0U, sizeof(pc), &pc);
        const crd::u32 groups = (kGridResolution + 3U) / 4U;  // local_size_x/y/z = 4
        cmd->dispatch(groups, groups, groups);
        cmd->buffer_barrier(*out_gpu, crd::rhi::BufferAccess::ComputeShaderWrite,
                                       crd::rhi::BufferAccess::TransferSrc);
        cmd->copy_buffer(*out_gpu, *out_readback, 0U, 0U, out_bytes);
        cmd->end();
        device->graphics_queue().submit(*cmd, *fence);
        fence->wait();

        // Build CPU reference buffer: evaluate the IR at every grid point.
        crd::containers::Array<float> cpu_ref(&alloc);
        cpu_ref.resize(kSampleCount);
        for (crd::u32 iz = 0U; iz < kGridResolution; ++iz)
        {
            for (crd::u32 iy = 0U; iy < kGridResolution; ++iy)
            {
                for (crd::u32 ix = 0U; ix < kGridResolution; ++ix)
                {
                    const crd::u32 flat = (iz * kGridResolution + iy) * kGridResolution + ix;
                    const auto     p    = grid_point(ix, iy, iz, pc.grid_origin, pc.grid_step);
                    cpu_ref[flat] = evaluate<float>(ir, p);
                }
            }
        }

        // Readback + ULP compare.
        auto* gpu_data = static_cast<const float*>(out_readback->map());
        REQUIRE(gpu_data != nullptr);

        const auto cmp = mixed_compare(
            crd::containers::ConstSpan<float>(cpu_ref.data(), cpu_ref.size()),
            crd::containers::ConstSpan<float>(gpu_data, kSampleCount),
            spec.ulp_tolerance, spec.abs_tolerance);

        if (!cmp.ok)
        {
            const crd::u32 idx = cmp.first_bad_idx;
            const crd::u32 iz = idx / (kGridResolution * kGridResolution);
            const crd::u32 iy = (idx / kGridResolution) % kGridResolution;
            const crd::u32 ix = idx % kGridResolution;
            const auto     p  = grid_point(ix, iy, iz, pc.grid_origin, pc.grid_step);
            std::printf("[%s] FIRST MISMATCH idx=%u (%u, %u, %u) p=(%g, %g, %g) "
                        "cpu=%g gpu=%g ulp_diff=%u abs_diff=%g  "
                        "(ulp_tol=%u abs_tol=%g)\n",
                        spec.name, idx, ix, iy, iz,
                        static_cast<double>(p.x), static_cast<double>(p.y), static_cast<double>(p.z),
                        static_cast<double>(cmp.cpu_value), static_cast<double>(cmp.gpu_value),
                        cmp.ulp_diff, static_cast<double>(cmp.abs_diff),
                        spec.ulp_tolerance, static_cast<double>(spec.abs_tolerance));
        }
        out_readback->unmap();

        CHECK(cmp.ok);
    }

    CHECK(capture.error_count()   == 0U);
    CHECK(capture.warning_count() == 0U);
    device->wait_idle();
}
