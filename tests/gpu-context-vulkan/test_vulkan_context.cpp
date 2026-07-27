// test_vulkan_context.cpp — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own,
// with a compute queue + (on capable adapters) the coopmat2 tensor feature — no rendering RHI, no swapchain. This is
// the foundation kir-vulkan migrates onto in v17-i-b.

#include <crd/gpu/vulkan_compute_context.hpp> // B-cmp: create_pipeline_from_spirv + the portable dispatch surface
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_shader_compile.hpp>

#include <crd/draw/draw_assets.hpp>  // REN-38-F7: the AUTHORED draw suite (the overlay-draw seam gate)
#include <crd/kir/ckir_cook.hpp>
#include <crd/kir/ckir_material.hpp>
#include <crd/kir/ckir_technique.hpp>
#include <crd/matcook/material_asset.hpp>
#include <crd/vertexcook/vertex_asset.hpp>
#include <crd/draw/overlay_pass.hpp> // RET-6 pt 3: submit_overlay (the GPU half under test)
#include <crd/draw/render_buffer.hpp>
#include <crd/draw/renderer.hpp>
#include <crd/kir/ckir.hpp>      // C1-c: create_program(KGraph, KEntry) — the IR on-ramp
#include <crd/kir/ckir_fft.hpp>    // B-cmp Phase 1: build_fft1d_radix2 (the CKIR FFT authoring layer)
#include <crd/kir/ckir_ocean.hpp>  // B16-a: the FFT-ocean kernels (spectrum/evolve/assemble)
#include <crd/kir/ckir_reduce.hpp> // B-cmp: build_reduce (the CKIR device-wide reduction)
#include <crd/kir/ckir_autotune.hpp> // AS-4: enumerate_reduce_schedules — the auto-scheduler generalized to the reduction
#include <crd/kir/ckir_scan.hpp>   // B-cmp: build_scan (the CKIR device-wide prefix sum)
#include <crd/kir/ckir_sort.hpp>   // B-cmp: build_sort_* (the CKIR stable LSD radix sort)
#include <crd/kir/ckir_mlp.hpp>    // v17 NRC: build_mlp_fwd_fp32 (the portable+bit-exact fused-MLP forward)
#include <crd/kir/ckir_neural.hpp> // B10: emit_coopvec_mlp_glsl (the per-invocation cooperative-vector neural-shading MLP)
#include <crd/kir/ckir_kernel_eval.hpp> // B11: eval_cpu_kernel (the CPU oracle for the wave/subgroup ops)
#include <ckir_subgroup_test.hpp>       // B11: build_subgroup_ops_kernel (the shared reduce/scan/broadcast/shuffle kernel)
#include <crd/kir/ckir_svgf.hpp>   // B14-c: build_svgf_atrous (the SVGF edge-stopping denoiser)
#include <crd/kir/ckir_ddgi.hpp>   // B14-b: build_ddgi_sample (the DDGI probe-sampling GI lookup)
#include <crd/kir/ckir_restir.hpp> // B14-a: build_restir_ris/temporal (the ReSTIR reservoir/RIS estimator)
#include <crd/kir/ckir_atmosphere.hpp> // B15-a: build_atmos_transmittance (the Hillaire/Bruneton sky-atmosphere LUTs)
#include <crd/kir/ckir_nrc.hpp>        // B14-d: build_nrc_hashgrid_encode (the Instant-NGP hash-grid encoder for the NRC)
#include <crd/kir/ckir_clouds.hpp>     // B15-b: build_cloud_density (the Nubis volumetric-cloud density field)
#include <crd/kir/ckir_hair.hpp>       // B18-a: build_hair_bcsdf_kernel (the Chiang R/TT/TRT/TRRT hair/fur BCSDF)
#include <crd/kir/ckir_lss.hpp>       // B18-f: the analytic linear-swept-sphere strand intersector
#include <crd/kir/ckir_hair_geom.hpp>   // B18-e: build_hair_filter_kernel (the tangent-oriented compositing filter)
#include <crd/kir/ckir_hair_scatter.hpp> // B18-c: the multiple-scattering tiers (moment LUT, dual scattering, DOM, volumetric)
#include <crd/kir/ckir_glsl.hpp> // B-cmp: emit_compute_kernel_glsl (the shared-memory compute-kernel emitter)
#include <crd/kir/ckir_hlsl.hpp> // B3-d: emit_stage_hlsl (the HLSL VS/FS emitter)
#include <crd/kir/ckir_visbuffer.hpp> // B4-vis: build_sw_raster_visbuffer (the compute software rasterizer)

#include <crd/math/cmath.hpp> // Phase-1 FFT: host-side twiddle table (cos/sin)
#include <crd/math/float_convert.hpp> // C6-b: f32<->f16 bit conversion for the cooperative-vector oracle

#include <crd/shadercook/cook.hpp>  // D-007 D2: the offline shader cook (CKIR graph -> .crdr bundle) under test
#include <crd/shadercook/variant.hpp> // D-007 D3: the variant/permutation system (matrix + content-hash dedup + on-demand)
#include <crd/shadercook/reload.hpp>  // D-007 D5: ReloadableCompute (hot-reload — recook + atomic pipeline swap)
#include <crd/shadercook/warmup.hpp>  // D-007 D11: AsyncPipelineWarmer (warm pipelines off the render thread on crd-jobs)
#include <crd/jobs/jobs.hpp>          // D-007 D10/D11: the fiber scheduler (jobs::init/shutdown/parallel_for)
#include <crd/kir/ckir_serialize.hpp> // D2: ShaderReflection (the IR-derived reflection carried in the bundle)
#include <crd/kir/ckir_material.hpp>  // materials: the OpenPBR surface slab (define_surface/build_surface/pack_gbuffer)
#include <crd/kir/ckir_cook.hpp>      // materials: SurfaceInputs + specialize_variant (the material variant seam)
#include <crd/platform/filesystem.hpp> // D2: create the cook cache dir
#include <ckir_kernel_dispatch.hpp> // B-cmp: the SHARED both-backend kernel dispatch + oracle-compare harness
#include <ckir_raster_triangle.hpp> // B3-e: the SHARED, backend-neutral CKIR triangle (identical on Vulkan + DX12)
#include <ckir_vertex_pull.hpp>     // GEO-1: the vertex-pulling VS (cooked MeshResource stream fetched by VertexIndex)
#include <crd/cooker/cook_handler.hpp> // GEO-1: the wave1 (.stl/.obj/.ply) cook handler — the import→cook→draw gate
#include <crd/cooker/cook_io.hpp>      // GEO-6: the declared-input seam every cook reads through
#include <crd/resources/crdr.hpp>      // GEO-1: crdr_read (the cooked MESH artifact readback)
#include <crd/resources/deflate.hpp>          // GEO-3 close: OUR zlib deflate (the in-test PNG fixture)
#include <crd/resources/png_image.hpp>        // GEO-3 close: OUR crc32 (the in-test PNG fixture)
#include <crd/resources/openpbr_material.hpp> // GEO-3 close: the authored 'PBRM' material (params + texture slots)
#include <crd/scene/serialize.hpp>            // GEO-3 close: kFourCC_SCEN (the decomposed scene artifact)
#include "win32_test_window.hpp"              // RET-2: the isolated real-window helper for the present gate
#include <crd/gpu/vulkan_validation_capture.hpp> // RET-4: the capture PORTED onto gpu-context (the rhi original dies)
#include <crd/imgui/imgui_gpu_backend.hpp>       // RET-5: the ImGui render backend on gpu-context
#include <imgui.h>                               // RET-5: driving a windowless ImGui context through the overlay gate
#include <ckir_visbuffer_test.hpp>  // B4-vis: the SHARED software-rasterizer scene + oracle + mixed-dtype dispatch
#include <ckir_oit_test.hpp>        // B17: the SHARED order-independent-transparency shaders + CPU oracle (WBOIT/...)
#include <ckir_abuffer_test.hpp>    // B17-c: the SHARED exact-reference A-buffer OIT scene + oracle + 2-kernel dispatch

#include <crd/containers/span.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cmath>   // std::lround for the god-ray tone quantisation
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

// D-007 C6: cooperative-VECTOR device enable (VK_NV_cooperative_vector) — the PER-INVOCATION matrix×vector inference primitive
// for neural shading (each pixel/thread runs a small MLP inline), the device half of the B10 moat. This gate proves the device
// comes up with the extension + feature ENABLED (a legal feature request under validation), reports the queried capabilities, and
// exercises the type-combination PROPERTY query. Portable: soft-skips on an adapter without the extension.
TEST_CASE("D-007 C6: cooperative-vector device enable (VK_NV_cooperative_vector) -- the B10 neural-shading device half",
          "[gpu-context][vulkan][gpu][coopvec]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    REQUIRE(vk->valid());
    std::printf("[coopvec] adapter=%s  coopvec=%s  training=%s  max_components=%u  stages=0x%x\n", vk->adapter_name(),
                vk->cooperative_vector() ? "YES" : "no", vk->cooperative_vector_training() ? "YES" : "no",
                vk->coopvec_max_components(), vk->coopvec_supported_stages());
    if (!vk->cooperative_vector())
    {
        WARN("adapter has no VK_NV_cooperative_vector -- the B10 neural-shading tier will be unavailable");
        return;
    }

    // the device CREATED successfully with VK_NV_cooperative_vector + cooperativeVector=VK_TRUE chained ⇒ the feature request was
    // legal (a validation layer would have failed device creation otherwise). Capabilities came from the feature/property query.
    CHECK(vk->vk_device() != VK_NULL_HANDLE);
    CHECK(vk->coopvec_max_components() > 0U);                                             // a real max cooperative-vector dimension
    CHECK((vk->coopvec_supported_stages() & VK_SHADER_STAGE_COMPUTE_BIT) != 0U);          // compute always supports coopvec

    // PROPERTY QUERY: the supported {input, matrix, bias, result} TYPE COMBINATIONS for the matrix×vector op (proc-loaded — this
    // is a physical-device extension function). nullptr ⇒ just the count; assert the device advertises at least one combo.
    auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeVectorPropertiesNV>(
        vkGetInstanceProcAddr(vk->vk_instance(), "vkGetPhysicalDeviceCooperativeVectorPropertiesNV"));
    REQUIRE(pfn != nullptr);
    crd::u32 combo_count = 0U;
    REQUIRE(pfn(vk->vk_physical_device(), &combo_count, nullptr) == VK_SUCCESS);
    CHECK(combo_count > 0U);
    std::printf("[coopvec] supported matrix-vector type combinations: %u\n", combo_count);
}

// D-007 C6-b: the cooperative-vector PROGRAM PATH — a real per-invocation matrix×vector (the inference primitive B10 builds on)
// DISPATCHES on the device and matches a CPU oracle. Each of B threads loads its own K-vector, multiplies it by a shared M×K
// fp16 weight matrix (RowMajor, fp32 accumulate — the standard inference combo), and stores its M-vector. Matched-accuracy vs a
// f16-rounded fp32 oracle (NOT bit-exact — tensor-core matmul reorders the sum; the FP32-precise CKIR tier owns bit-exactness).
TEST_CASE("D-007 C6-b: cooperative-vector matrix-vector DISPATCHES on Vulkan == fp16 oracle (the B10 program path)",
          "[gpu-context][vulkan][gpu][coopvec]")
{
    namespace cg = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector; skipping"); return; }
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    static const char* const kCoopVecSrc =
        "#version 460\n"
        "#extension GL_NV_cooperative_vector : require\n"
        "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n"
        "#extension GL_EXT_shader_explicit_arithmetic_types : require\n"
        "layout(local_size_x = 64) in;\n"
        "layout(set=0, binding=0) readonly  buffer InBuf  { float16_t data[]; } inb;\n"
        "layout(set=0, binding=1) readonly  buffer MatBuf { float16_t data[]; } matb;\n"
        "layout(set=0, binding=2) writeonly buffer OutBuf { float16_t data[]; } outb;\n"
        "layout(set=0, binding=3) readonly  buffer CfgBuf { uint      data[]; } cfg;\n"
        "void main() {\n"
        "    uint tid = gl_GlobalInvocationID.x;\n"
        "    uint B = cfg.data[0];\n"
        "    if (tid >= B) { return; }\n"
        "    const uint K = 16u; const uint M = 16u;\n"
        "    coopvecNV<float16_t, 16> v;\n"
        "    coopVecLoadNV(v, inb.data, tid * K * 2u);\n" // load offset: BYTES (float16_t buffer)
        "    coopvecNV<float16_t, 16> r;\n"               // fp16 result (the only fp16-input combo the HW supports)
        "    coopVecMatMulNV(r, v, gl_ComponentTypeFloat16NV, matb.data, 0u, gl_ComponentTypeFloat16NV,\n"
        "                    M, K, gl_CooperativeVectorMatrixLayoutRowMajorNV, false, K * 2u);\n" // matrixStride: BYTES/row
        "    coopVecStoreNV(r, outb.data, tid * M * 2u);\n" // store offset: BYTES (float16_t buffer)

        "}\n";

    const auto spv =
        gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::StringView(kCoopVecSrc), "coopvec_matvec", &alloc);
    if (!spv.ok) { WARN("coopvec GLSL->SPIR-V failed: " << spv.error_message.c_str()); }
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 4, 0U);
    REQUIRE(pipe != nullptr);
    // NOTE: on this HW the only fp16-input matmul combination is {input f16, matrix f16, result f16} (the integer combos are
    // int8/fp8 quantized inference). The RowMajor layout multiplies correctly with tightly-packed weights — no optimal-layout
    // conversion is needed for CORRECTNESS (the optimal layout is a later perf lever).

    constexpr int b_n = 256;
    constexpr int k_n = 16;
    constexpr int m_n = 16;
    crd::containers::Array<crd::u16> in_h(&alloc);  in_h.resize(uz(b_n * k_n));
    crd::containers::Array<crd::u16> w_h(&alloc);   w_h.resize(uz(m_n * k_n));
    // deterministic pseudo-random in ~[-1,1], stored as the f16-ROUNDED values (both device and oracle read these exact bits).
    const auto rnd = [](int i) { const crd::u32 h = (static_cast<crd::u32>(i) * 2654435761U) ^ 0x9E3779B9U; return (static_cast<float>(h & 0xFFFFU) / 32768.0F) - 1.0F; };
    for (int i = 0; i < b_n * k_n; ++i) { in_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i)); }
    for (int i = 0; i < m_n * k_n; ++i) { w_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i + 991)); }
    // oracle: accumulate in fp32 (using the f16-rounded operands), then ROUND the result to fp16 — the device stores an fp16
    // result (combo 0), so the comparison is against the f16-rounded sum. Matched-accuracy, not bit-exact.
    crd::containers::Array<float> ref(&alloc); ref.resize(uz(b_n * m_n), 0.0F);
    for (int b = 0; b < b_n; ++b)
    {
        for (int m = 0; m < m_n; ++m)
        {
            float acc = 0.0F;
            for (int k = 0; k < k_n; ++k) { acc += crd::math::f16_bits_to_f32(in_h[uz(b * k_n + k)]) * crd::math::f16_bits_to_f32(w_h[uz(m * k_n + k)]); }
            ref[uz(b * m_n + m)] = crd::math::f16_bits_to_f32(crd::math::f32_to_f16_bits(acc));
        }
    }

    auto d_in  = compute.create_buffer(static_cast<crd::u64>(b_n * k_n) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_w   = compute.create_buffer(static_cast<crd::u64>(m_n * k_n) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out = compute.create_buffer(static_cast<crd::u64>(b_n * m_n) * 2U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_cfg = compute.create_buffer(4U * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    const auto up_bytes = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
        auto stg  = compute.create_buffer(nbytes, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<crd::u8*>(stg->map());
        const auto* s = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = s[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nbytes);
        compute.submit_and_wait();
    };
    const crd::u32 cfgv[4] = {static_cast<crd::u32>(b_n), static_cast<crd::u32>(k_n), static_cast<crd::u32>(m_n), 0U};
    up_bytes(*d_in, in_h.data(), static_cast<crd::u64>(b_n * k_n) * 2U);
    up_bytes(*d_w, w_h.data(), static_cast<crd::u64>(m_n * k_n) * 2U);
    up_bytes(*d_cfg, cfgv, 4U * 4U);

    auto& rec = compute.begin();
    cg::ComputeBuffer* binds[4] = {d_in.get(), d_w.get(), d_out.get(), d_cfg.get()};
    rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 4), nullptr, 0U, static_cast<crd::u32>((b_n + 63) / 64), 1U, 1U);
    rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(b_n * m_n) * 2U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    {
        auto& r2 = compute.begin();
        r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(b_n * m_n) * 2U);
        compute.submit_and_wait();
    }
    const auto* out = static_cast<const crd::u16*>(rb->map()); // fp16 bits
    float worst = 0.0F;
    for (int i = 0; i < b_n * m_n; ++i) { const float d0 = crd::math::f16_bits_to_f32(out[uz(i)]) - ref[uz(i)]; const float d = d0 < 0.0F ? -d0 : d0; if (d > worst) { worst = d; } }
    rb->unmap();
    std::printf("[coopvec] matvec B=%d K=%d M=%d  worst |device - oracle| = %.5f\n", b_n, k_n, m_n, static_cast<double>(worst));
    CHECK(worst < 0.06F); // matched fp16 accuracy (fp16 products + fp16-rounded result; the HW accumulation order differs from the oracle's)
}

// D-007 B10: the NEURAL-SHADING MOAT — a per-invocation multi-layer MLP (crd::kir::neural::emit_coopvec_mlp_glsl) runs inline on
// the cooperative-vector tensor units and matches the CPU reference. This is the substrate for per-pixel neural materials/textures:
// each thread feeds its feature vector through a 16→16→16→16 ReLU MLP (2 hidden + linear output). Matched fp16 accuracy.
TEST_CASE("D-007 B10: cooperative-vector MLP (neural shading) DISPATCHES on Vulkan == fp16 reference",
          "[gpu-context][vulkan][gpu][coopvec][neural]")
{
    namespace cg  = crd::gpu;
    namespace kir = crd::kir;
    namespace nn  = crd::kir::neural;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector; skipping"); return; }
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    nn::CoopVecMlpConfig mlp;
    mlp.in_dim = 16; mlp.hidden = 16; mlp.out_dim = 16; mlp.hidden_layers = 2;
    kir::GlslKernel kern(&alloc);
    REQUIRE(nn::emit_coopvec_mlp_glsl(mlp, kern));
    const auto spv =
        gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "coopvec_mlp", &alloc);
    if (!spv.ok) { WARN("coopvec MLP GLSL->SPIR-V failed: " << spv.error_message.c_str()); }
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
    REQUIRE(pipe != nullptr);

    constexpr int n_s = 256;
    const int     wc  = mlp.weight_count();
    const int     bc  = mlp.bias_count();
    crd::containers::Array<crd::u16> in_h(&alloc);  in_h.resize(uz(n_s * mlp.in_dim));
    crd::containers::Array<crd::u16> w_h(&alloc);   w_h.resize(uz(wc));
    crd::containers::Array<crd::u16> b_h(&alloc);   b_h.resize(uz(bc));
    // small deterministic pseudo-random weights/inputs (scaled so activations stay in fp16's accurate range, no ReLU-dead net)
    const auto rnd = [](int i, float scale) { const crd::u32 h = (static_cast<crd::u32>(i) * 2654435761U) ^ 0x9E3779B9U; return ((static_cast<float>(h & 0xFFFFU) / 32768.0F) - 1.0F) * scale; };
    for (int i = 0; i < n_s * mlp.in_dim; ++i) { in_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i, 1.0F)); }
    for (int i = 0; i < wc; ++i) { w_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i + 17, 0.25F)); }   // 1/sqrt(16) ~ 0.25 init
    for (int i = 0; i < bc; ++i) { b_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i + 7919, 0.1F)); }
    crd::containers::Array<crd::u16> ref(&alloc); ref.resize(uz(n_s * mlp.out_dim), static_cast<crd::u16>(0));
    nn::eval_coopvec_mlp_cpu(mlp, w_h.data(), b_h.data(), in_h.data(), n_s, ref.data());

    auto d_in  = compute.create_buffer(static_cast<crd::u64>(n_s * mlp.in_dim) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_w   = compute.create_buffer(static_cast<crd::u64>(wc) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_b   = compute.create_buffer(static_cast<crd::u64>(bc) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out = compute.create_buffer(static_cast<crd::u64>(n_s * mlp.out_dim) * 2U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_cfg = compute.create_buffer(4U * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    const auto up_bytes = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
        auto  stg = compute.create_buffer(nbytes, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<crd::u8*>(stg->map());
        const auto* srcb = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = srcb[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nbytes);
        compute.submit_and_wait();
    };
    const crd::u32 cfgv[4] = {static_cast<crd::u32>(n_s), 0U, 0U, 0U};
    up_bytes(*d_in, in_h.data(), static_cast<crd::u64>(n_s * mlp.in_dim) * 2U);
    up_bytes(*d_w, w_h.data(), static_cast<crd::u64>(wc) * 2U);
    up_bytes(*d_b, b_h.data(), static_cast<crd::u64>(bc) * 2U);
    up_bytes(*d_cfg, cfgv, 4U * 4U);

    auto& rec = compute.begin();
    cg::ComputeBuffer* binds[5] = {d_in.get(), d_w.get(), d_b.get(), d_out.get(), d_cfg.get()};
    rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 5), nullptr, 0U, static_cast<crd::u32>((n_s + 63) / 64), 1U, 1U);
    rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(n_s * mlp.out_dim) * 2U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    {
        auto& r2 = compute.begin();
        r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(n_s * mlp.out_dim) * 2U);
        compute.submit_and_wait();
    }
    const auto* out = static_cast<const crd::u16*>(rb->map());
    float worst = 0.0F;
    for (int i = 0; i < n_s * mlp.out_dim; ++i) { const float d0 = crd::math::f16_bits_to_f32(out[uz(i)]) - crd::math::f16_bits_to_f32(ref[uz(i)]); const float d = d0 < 0.0F ? -d0 : d0; if (d > worst) { worst = d; } }
    rb->unmap();
    std::printf("[coopvec] MLP %d->%dx%d->%d  N=%d  worst |device - reference| = %.5f\n", mlp.in_dim, mlp.hidden, mlp.hidden_layers, mlp.out_dim, n_s, static_cast<double>(worst));
    CHECK(worst < 0.05F); // matched fp16 accuracy across 3 layers
}

// D-007 B10 PERFORMANCE: the neural-shading CRUSH — run a small MLP PER PIXEL at 1080p (2,073,600 invocations) on the cooperative-
// vector tensor units vs the identical MLP as a hand-written scalar-FMA shader. GPU-timestamped (min-of-6), self-verifying (both
// compute the same MLP ⇒ outputs match to fp16). This is the moat: real-time per-pixel neural materials/textures. Hidden ([.]).
TEST_CASE("D-007 B10: cooperative-vector MLP CRUSH -- per-pixel neural shading vs a scalar-FMA baseline (GPU-timed)",
          "[.coopvec-bench]")
{
    namespace cg  = crd::gpu;
    namespace kir = crd::kir;
    namespace nn  = crd::kir::neural;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector; skipping"); return; }
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(512U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    nn::CoopVecMlpConfig mlp;
    mlp.in_dim = 16; mlp.hidden = 32; mlp.out_dim = 16; mlp.hidden_layers = 3; // a realistic per-pixel neural material MLP
    kir::GlslKernel kcv(&alloc);
    kir::GlslKernel ksc(&alloc);
    REQUIRE(nn::emit_coopvec_mlp_glsl(mlp, kcv));
    REQUIRE(nn::emit_scalar_mlp_glsl(mlp, ksc));
    const auto spv_cv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kcv.source), "cv_mlp", &alloc);
    const auto spv_sc = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(ksc.source), "sc_mlp", &alloc);
    REQUIRE(spv_cv.ok);
    REQUIRE(spv_sc.ok);
    auto pipe_cv = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv_cv.spirv.data(), spv_cv.spirv.size()), 5, 0U);
    auto pipe_sc = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv_sc.spirv.data(), spv_sc.spirv.size()), 5, 0U);
    REQUIRE(pipe_cv != nullptr);
    REQUIRE(pipe_sc != nullptr);

    constexpr int n_px = 1920 * 1080; // one MLP evaluation per pixel at 1080p
    const int     wc   = mlp.weight_count();
    const int     bc   = mlp.bias_count();
    crd::containers::Array<crd::u16> in_h(&alloc);  in_h.resize(uz(n_px * mlp.in_dim));
    crd::containers::Array<crd::u16> w_h(&alloc);   w_h.resize(uz(wc));
    crd::containers::Array<crd::u16> b_h(&alloc);   b_h.resize(uz(bc));
    const auto rnd = [](int i, float scale) { const crd::u32 h = (static_cast<crd::u32>(i) * 2654435761U) ^ 0x9E3779B9U; return ((static_cast<float>(h & 0xFFFFU) / 32768.0F) - 1.0F) * scale; };
    for (int i = 0; i < n_px * mlp.in_dim; ++i) { in_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i, 1.0F)); }
    for (int i = 0; i < wc; ++i) { w_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i + 17, 0.18F)); }
    for (int i = 0; i < bc; ++i) { b_h[uz(i)] = crd::math::f32_to_f16_bits(rnd(i + 7919, 0.1F)); }

    auto d_in   = compute.create_buffer(static_cast<crd::u64>(n_px * mlp.in_dim) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_w    = compute.create_buffer(static_cast<crd::u64>(wc) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_b    = compute.create_buffer(static_cast<crd::u64>(bc) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_ocv  = compute.create_buffer(static_cast<crd::u64>(n_px * mlp.out_dim) * 2U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_osc  = compute.create_buffer(static_cast<crd::u64>(n_px * mlp.out_dim) * 2U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_cfg  = compute.create_buffer(4U * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    const auto up_bytes = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
        auto  stg = compute.create_buffer(nbytes, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<crd::u8*>(stg->map());
        const auto* srcb = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = srcb[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nbytes);
        compute.submit_and_wait();
    };
    const crd::u32 cfgv[4] = {static_cast<crd::u32>(n_px), 0U, 0U, 0U};
    up_bytes(*d_in, in_h.data(), static_cast<crd::u64>(n_px * mlp.in_dim) * 2U);
    up_bytes(*d_w, w_h.data(), static_cast<crd::u64>(wc) * 2U);
    up_bytes(*d_b, b_h.data(), static_cast<crd::u64>(bc) * 2U);
    up_bytes(*d_cfg, cfgv, 4U * 4U);

    const crd::u32 groups = static_cast<crd::u32>((n_px + 63) / 64);
    const auto     run    = [&](cg::ComputePipeline& pipe, cg::ComputeBuffer& d_out) {
        auto& rec = compute.begin();
        cg::ComputeBuffer* binds[5] = {d_in.get(), d_w.get(), d_b.get(), &d_out, d_cfg.get()};
        rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 5), nullptr, 0U, groups, 1U, 1U);
        rec.barrier(d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        compute.submit_and_wait();
    };
    double best_cv = 1.0e30;
    double best_sc = 1.0e30;
    run(*pipe_cv, *d_ocv); // warmup
    run(*pipe_sc, *d_osc);
    for (int r = 0; r < 6; ++r) { run(*pipe_cv, *d_ocv); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best_cv) { best_cv = ms; } }
    for (int r = 0; r < 6; ++r) { run(*pipe_sc, *d_osc); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best_sc) { best_sc = ms; } }

    // self-verify: both kernels compute the SAME MLP ⇒ their fp16 outputs must match (a modest tolerance for the different
    // accumulation paths — tensor-core vs scalar fp32).
    auto rb_cv = compute.create_buffer(static_cast<crd::u64>(n_px * mlp.out_dim) * 2U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    auto rb_sc = compute.create_buffer(static_cast<crd::u64>(n_px * mlp.out_dim) * 2U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r2 = compute.begin(); r2.copy(*d_ocv, *rb_cv, 0U, 0U, static_cast<crd::u64>(n_px * mlp.out_dim) * 2U); r2.copy(*d_osc, *rb_sc, 0U, 0U, static_cast<crd::u64>(n_px * mlp.out_dim) * 2U); compute.submit_and_wait(); }
    const auto* ocv = static_cast<const crd::u16*>(rb_cv->map());
    const auto* osc = static_cast<const crd::u16*>(rb_sc->map());
    float worst = 0.0F;
    for (int i = 0; i < 4096; ++i) { const int idx = (i * 509) % (n_px * mlp.out_dim); const float d0 = crd::math::f16_bits_to_f32(ocv[uz(idx)]) - crd::math::f16_bits_to_f32(osc[uz(idx)]); const float d = d0 < 0.0F ? -d0 : d0; if (d > worst) { worst = d; } }
    rb_cv->unmap();
    rb_sc->unmap();

    const double macs = static_cast<double>(n_px) * static_cast<double>(mlp.weight_count()); // ~one MAC per weight per pixel
    std::printf("\n=== B10 neural-shading CRUSH (RTX 4070 Ti SUPER) — MLP %d->%dx%d->%d, %d px (1080p) ===\n",
                mlp.in_dim, mlp.hidden, mlp.hidden_layers, mlp.out_dim, n_px);
    std::printf("  coopvec (tensor units): %.3f ms  (%.1f fps, %.1f GMAC/s)\n", best_cv, 1000.0 / best_cv, macs / best_cv / 1.0e6);
    std::printf("  scalar  (ALU FMA)     : %.3f ms  (%.1f fps, %.1f GMAC/s)\n", best_sc, 1000.0 / best_sc, macs / best_sc / 1.0e6);
    std::printf("  SPEEDUP (coopvec / scalar): %.2fx     [cross-check worst |cv - scalar| = %.4f]\n", best_sc / best_cv, static_cast<double>(worst));
    CHECK(best_cv > 0.0);
    CHECK(best_sc > 0.0);
    CHECK(worst < 0.20F); // both compute the same MLP
}

// D-007 B10: a NEURAL MATERIAL — a 2-D neural field (frequency-encoded uv → MLP → RGB) CPU-trained to reproduce a target pattern,
// then RENDERED per pixel on the cooperative-vector tensor path to a BMP. The tangible neural-shading deliverable: a learned
// texture evaluated inline in a shader. Reports PSNR (rendered vs target) + writes neural_material.bmp / neural_target.bmp. Hidden.
TEST_CASE("D-007 B10: NEURAL MATERIAL -- CPU-trained 2D neural field rendered per-pixel on the coopvec tensor path (-> BMP)",
          "[.neural-material]")
{
    namespace cg  = crd::gpu;
    namespace kir = crd::kir;
    namespace nn  = crd::kir::neural;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector; skipping"); return; }
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    nn::CoopVecMlpConfig mlp;
    mlp.in_dim = 16; mlp.hidden = 32; mlp.out_dim = 3; mlp.hidden_layers = 2; // 16(enc) -> 32 -> 32 -> 3(rgb)

    // the target pattern — a smooth colourful field the neural material learns to reproduce.
    const auto target = [](float u, float v, float* rgb) {
        const float s1 = crd::math::sin(6.2831853F * (u * 1.3F + 0.10F));
        const float s2 = crd::math::cos(6.2831853F * (v * 1.1F + 0.20F));
        const float s3 = crd::math::sin(6.2831853F * ((u + v) * 0.8F));
        rgb[0] = 0.5F + 0.35F * crd::math::sin(2.0F * s1 + s3);
        rgb[1] = 0.5F + 0.35F * crd::math::cos(2.0F * s2 - s3);
        rgb[2] = 0.5F + 0.35F * crd::math::sin(1.5F * (s1 + s2) + 0.5F);
    };

    // ── TRAIN the MLP on the CPU (fp32 full-batch gradient descent, 3-layer backprop) ──
    const int wc = mlp.weight_count();
    const int bc = mlp.bias_count();
    crd::containers::Array<float> w(&alloc);  w.resize(uz(wc), 0.0F);
    crd::containers::Array<float> bd(&alloc); bd.resize(uz(bc), 0.0F);
    const auto rnd = [](int i) { const crd::u32 h = (static_cast<crd::u32>(i) * 2654435761U) ^ 0x9E3779B9U; return (static_cast<float>(h & 0xFFFFU) / 32768.0F) - 1.0F; };
    for (int i = 0; i < 32 * 16; ++i) { w[uz(i)] = rnd(i) * 0.30F; }             // W0 (fan-in 16)
    for (int i = 0; i < 32 * 32; ++i) { w[uz(512 + i)] = rnd(i + 101) * 0.18F; } // W1 (fan-in 32)
    for (int i = 0; i < 3 * 32; ++i) { w[uz(1536 + i)] = rnd(i + 907) * 0.18F; } // W2
    const int   grid   = 32;
    const int   epochs = 1200;
    const float lr     = 0.01F; // Adam
    crd::containers::Array<float> gw(&alloc);  gw.resize(uz(wc), 0.0F);
    crd::containers::Array<float> gb(&alloc);  gb.resize(uz(bc), 0.0F);
    crd::containers::Array<float> mw(&alloc);  mw.resize(uz(wc), 0.0F);
    crd::containers::Array<float> vw(&alloc);  vw.resize(uz(wc), 0.0F);
    crd::containers::Array<float> mb(&alloc);  mb.resize(uz(bc), 0.0F);
    crd::containers::Array<float> vb(&alloc);  vb.resize(uz(bc), 0.0F);
    float b1t = 1.0F;
    float b2t = 1.0F;
    for (int ep = 0; ep < epochs; ++ep)
    {
        for (int i = 0; i < wc; ++i) { gw[uz(i)] = 0.0F; }
        for (int i = 0; i < bc; ++i) { gb[uz(i)] = 0.0F; }
        for (int gy = 0; gy < grid; ++gy)
        {
            for (int gx = 0; gx < grid; ++gx)
            {
                const float uu = (static_cast<float>(gx) + 0.5F) / static_cast<float>(grid);
                const float vv = (static_cast<float>(gy) + 0.5F) / static_cast<float>(grid);
                float a0[16];
                nn::neural_uv_encode(uu, vv, 16, a0);
                float z1[32];
                float a1[32];
                for (int r = 0; r < 32; ++r) { float acc = bd[uz(r)]; for (int k = 0; k < 16; ++k) { acc += w[uz(r * 16 + k)] * a0[k]; } z1[r] = acc; a1[r] = acc > 0.0F ? acc : 0.0F; }
                float z2[32];
                float a2[32];
                for (int r = 0; r < 32; ++r) { float acc = bd[uz(32 + r)]; for (int k = 0; k < 32; ++k) { acc += w[uz(512 + r * 32 + k)] * a1[k]; } z2[r] = acc; a2[r] = acc > 0.0F ? acc : 0.0F; }
                float out3[3];
                for (int o = 0; o < 3; ++o) { float acc = bd[uz(64 + o)]; for (int k = 0; k < 32; ++k) { acc += w[uz(1536 + o * 32 + k)] * a2[k]; } out3[o] = acc; }
                float tgt[3];
                target(uu, vv, tgt);
                float d3[3];
                for (int o = 0; o < 3; ++o) { d3[o] = 2.0F * (out3[o] - tgt[o]); }
                for (int o = 0; o < 3; ++o) { gb[uz(64 + o)] += d3[o]; for (int k = 0; k < 32; ++k) { gw[uz(1536 + o * 32 + k)] += d3[o] * a2[k]; } }
                float d2[32];
                for (int k = 0; k < 32; ++k) { float acc = 0.0F; for (int o = 0; o < 3; ++o) { acc += w[uz(1536 + o * 32 + k)] * d3[o]; } d2[k] = z2[k] > 0.0F ? acc : 0.0F; }
                for (int r = 0; r < 32; ++r) { gb[uz(32 + r)] += d2[r]; for (int k = 0; k < 32; ++k) { gw[uz(512 + r * 32 + k)] += d2[r] * a1[k]; } }
                float d1[32];
                for (int k = 0; k < 32; ++k) { float acc = 0.0F; for (int r = 0; r < 32; ++r) { acc += w[uz(512 + r * 32 + k)] * d2[r]; } d1[k] = z1[k] > 0.0F ? acc : 0.0F; }
                for (int r = 0; r < 32; ++r) { gb[uz(r)] += d1[r]; for (int k = 0; k < 16; ++k) { gw[uz(r * 16 + k)] += d1[r] * a0[k]; } }
            }
        }
        b1t *= 0.9F;
        b2t *= 0.999F;
        const float bc1 = 1.0F - b1t;
        const float bc2 = 1.0F - b2t;
        const float ns  = 1.0F / static_cast<float>(grid * grid);
        for (int i = 0; i < wc; ++i) { const float g = gw[uz(i)] * ns; mw[uz(i)] = 0.9F * mw[uz(i)] + 0.1F * g; vw[uz(i)] = 0.999F * vw[uz(i)] + 0.001F * g * g; w[uz(i)] -= lr * (mw[uz(i)] / bc1) / (crd::math::sqrt(vw[uz(i)] / bc2) + 1.0e-8F); }
        for (int i = 0; i < bc; ++i) { const float g = gb[uz(i)] * ns; mb[uz(i)] = 0.9F * mb[uz(i)] + 0.1F * g; vb[uz(i)] = 0.999F * vb[uz(i)] + 0.001F * g * g; bd[uz(i)] -= lr * (mb[uz(i)] / bc1) / (crd::math::sqrt(vb[uz(i)] / bc2) + 1.0e-8F); }
    }

    // ISOLATION: PSNR of a CPU fp32 forward of the trained net (vs target) over a 128² grid — separates training from the fp16 render.
    {
        double cmse = 0.0;
        const int cg = 128;
        for (int gy = 0; gy < cg; ++gy)
        {
            for (int gx = 0; gx < cg; ++gx)
            {
                const float uu = (static_cast<float>(gx) + 0.5F) / static_cast<float>(cg);
                const float vv = (static_cast<float>(gy) + 0.5F) / static_cast<float>(cg);
                float a0[16]; nn::neural_uv_encode(uu, vv, 16, a0);
                float a1[32]; for (int r = 0; r < 32; ++r) { float acc = bd[uz(r)]; for (int k = 0; k < 16; ++k) { acc += w[uz(r * 16 + k)] * a0[k]; } a1[r] = acc > 0.0F ? acc : 0.0F; }
                float a2[32]; for (int r = 0; r < 32; ++r) { float acc = bd[uz(32 + r)]; for (int k = 0; k < 32; ++k) { acc += w[uz(512 + r * 32 + k)] * a1[k]; } a2[r] = acc > 0.0F ? acc : 0.0F; }
                float o3[3]; for (int o = 0; o < 3; ++o) { float acc = bd[uz(64 + o)]; for (int k = 0; k < 32; ++k) { acc += w[uz(1536 + o * 32 + k)] * a2[k]; } o3[o] = acc; }
                float t[3]; target(uu, vv, t);
                for (int o = 0; o < 3; ++o) { cmse += static_cast<double>((o3[o] - t[o]) * (o3[o] - t[o])); }
            }
        }
        cmse /= static_cast<double>(cg * cg * 3);
        std::printf("[neural-material] CPU fp32 forward PSNR = %.2f dB (isolates training from the fp16 render)\n", 10.0 * crd::math::log10(1.0 / cmse));
    }

    // quantize the trained weights to fp16 for the tensor render.
    crd::containers::Array<crd::u16> w16(&alloc); w16.resize(uz(wc));
    crd::containers::Array<crd::u16> b16(&alloc); b16.resize(uz(bc));
    for (int i = 0; i < wc; ++i) { w16[uz(i)] = crd::math::f32_to_f16_bits(w[uz(i)]); }
    for (int i = 0; i < bc; ++i) { b16[uz(i)] = crd::math::f32_to_f16_bits(bd[uz(i)]); }

    // ── RENDER the neural material per pixel on the coopvec tensor path ──
    kir::GlslKernel kern(&alloc);
    REQUIRE(nn::emit_neural_material_render_glsl(mlp, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "neural_mat", &alloc);
    if (!spv.ok) { WARN("neural material GLSL->SPIR-V failed: " << spv.error_message.c_str()); }
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 4, 0U);
    REQUIRE(pipe != nullptr);

    constexpr int dim = 512;
    auto d_w   = compute.create_buffer(static_cast<crd::u64>(wc) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_b   = compute.create_buffer(static_cast<crd::u64>(bc) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out = compute.create_buffer(static_cast<crd::u64>(dim * dim) * 4U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_cfg = compute.create_buffer(4U * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    const auto up_bytes = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
        auto  stg = compute.create_buffer(nbytes, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<crd::u8*>(stg->map());
        const auto* srcb = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = srcb[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nbytes);
        compute.submit_and_wait();
    };
    const crd::u32 cfgv[4] = {static_cast<crd::u32>(dim), static_cast<crd::u32>(dim), 0U, 0U};
    up_bytes(*d_w, w16.data(), static_cast<crd::u64>(wc) * 2U);
    up_bytes(*d_b, b16.data(), static_cast<crd::u64>(bc) * 2U);
    up_bytes(*d_cfg, cfgv, 4U * 4U);

    auto& rec = compute.begin();
    cg::ComputeBuffer* binds[4] = {d_w.get(), d_b.get(), d_out.get(), d_cfg.get()};
    rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 4), nullptr, 0U, static_cast<crd::u32>(dim / 8), static_cast<crd::u32>(dim / 8), 1U);
    rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(dim * dim) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r2 = compute.begin(); r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(dim * dim) * 4U); compute.submit_and_wait(); }
    const auto* img = static_cast<const crd::u32*>(rb->map());

    // PSNR (rendered neural material vs the target) + write both BMPs.
    double mse = 0.0;
    for (int y = 0; y < dim; ++y)
    {
        for (int x = 0; x < dim; ++x)
        {
            const crd::u32 pv = img[uz(y * dim + x)];
            const float    nr = static_cast<float>(pv & 0xFFU) / 255.0F;
            const float    ng = static_cast<float>((pv >> 8U) & 0xFFU) / 255.0F;
            const float    nb = static_cast<float>((pv >> 16U) & 0xFFU) / 255.0F;
            float          tgt[3];
            target((static_cast<float>(x) + 0.5F) / dim, (static_cast<float>(y) + 0.5F) / dim, tgt);
            mse += static_cast<double>((nr - tgt[0]) * (nr - tgt[0]) + (ng - tgt[1]) * (ng - tgt[1]) + (nb - tgt[2]) * (nb - tgt[2]));
        }
    }
    mse /= static_cast<double>(dim * dim * 3);
    const double psnr = 10.0 * crd::math::log10(1.0 / mse);

    const auto write_bmp = [&](const char* path, bool from_target) {
        const crd::u32 rowsize = (static_cast<crd::u32>(dim) * 3U + 3U) & ~3U;
        crd::containers::Array<unsigned char> bmp(&alloc);
        bmp.resize(54U + static_cast<crd::usize>(rowsize) * static_cast<crd::u32>(dim), static_cast<unsigned char>(0));
        const auto p4 = [&](crd::u32 o, crd::u32 vv) { bmp[o] = static_cast<unsigned char>(vv & 0xFFU); bmp[o + 1] = static_cast<unsigned char>((vv >> 8U) & 0xFFU); bmp[o + 2] = static_cast<unsigned char>((vv >> 16U) & 0xFFU); bmp[o + 3] = static_cast<unsigned char>((vv >> 24U) & 0xFFU); };
        bmp[0] = 'B'; bmp[1] = 'M';
        p4(2U, 54U + rowsize * static_cast<crd::u32>(dim)); p4(10U, 54U); p4(14U, 40U);
        p4(18U, static_cast<crd::u32>(dim)); p4(22U, static_cast<crd::u32>(dim)); bmp[26] = 1U; bmp[28] = 24U; p4(34U, rowsize * static_cast<crd::u32>(dim));
        for (int fy = 0; fy < dim; ++fy)
        {
            const int sy = dim - 1 - fy;
            for (int x = 0; x < dim; ++x)
            {
                float rr = 0.0F;
                float gg = 0.0F;
                float bbl = 0.0F;
                if (from_target) { float t[3]; target((static_cast<float>(x) + 0.5F) / dim, (static_cast<float>(sy) + 0.5F) / dim, t); rr = t[0]; gg = t[1]; bbl = t[2]; }
                else { const crd::u32 pv = img[uz(sy * dim + x)]; rr = static_cast<float>(pv & 0xFFU) / 255.0F; gg = static_cast<float>((pv >> 8U) & 0xFFU) / 255.0F; bbl = static_cast<float>((pv >> 16U) & 0xFFU) / 255.0F; }
                const auto q = [](float c) { float cc = c; if (cc < 0.0F) { cc = 0.0F; } else if (cc > 1.0F) { cc = 1.0F; } return static_cast<unsigned char>(crd::math::round(cc * 255.0F)); };
                const crd::u32 o = 54U + static_cast<crd::u32>(fy) * rowsize + static_cast<crd::u32>(x) * 3U;
                bmp[o] = q(bbl); bmp[o + 1] = q(gg); bmp[o + 2] = q(rr);
            }
        }
        FILE* f = nullptr;
#ifdef _MSC_VER
        if (fopen_s(&f, path, "wb") != 0) { f = nullptr; } // MSVC: the deprecated fopen errors under /WX
#else
        f = std::fopen(path, "wb"); // fopen_s is MSVC-only (the hair_render.hpp idiom)
#endif
        if (f != nullptr) { fwrite(bmp.data(), 1U, bmp.size(), f); fclose(f); }
    };
    write_bmp("D:/Dev/cerid/build/neural_material.bmp", false);
    write_bmp("D:/Dev/cerid/build/neural_target.bmp", true);
    rb->unmap();
    std::printf("[neural-material] trained %d->32->32->3 field, %dx%d render on tensor path -> PSNR %.2f dB (neural_material.bmp)\n", mlp.in_dim, dim, dim, psnr);
    CHECK(psnr > 32.0); // the neural field reproduces the target faithfully (Adam-trained; fp16 tensor render tracks the fp32 net)
}

// D-007 B10: ON-DEVICE DIFFERENTIABLE TRAINING — the moat's defining claim. A single linear layer y=Wx+b is TRAINED ON THE GPU to
// fit a known target map: each step the forward + the loss gradient + the WEIGHT-gradient outer product (coopVecOuterProductAccumulate)
// + the BIAS gradient (coopVecReduceSumAccumulate) all run on the cooperative-vector tensor path; the host converts the
// TrainingOptimal weight-grad → RowMajor (vkConvertCooperativeVectorMatrixNV) and applies SGD. Verifies (a) the on-device gradient
// == the CPU reference gradient (the hardware training op is correct) and (b) the loss CONVERGES to ~0 (it learns).
TEST_CASE("D-007 B10: on-device coopvec TRAINING -- hardware gradients fit a linear map, loss converges",
          "[gpu-context][vulkan][gpu][coopvec][neural][train]")
{
    namespace cg  = crd::gpu;
    namespace kir = crd::kir;
    namespace nn  = crd::kir::neural;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector; skipping"); return; }
    if (!vk->cooperative_vector_training()) { WARN("no cooperativeVectorTraining; skipping"); return; }
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(64U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    constexpr int d_n = 16;
    constexpr int n_s = 256;
    // the KNOWN target linear map W*,b* (small, so activations/gradients stay in fp16's accurate range) + random inputs → targets.
    const auto rnd = [](int i) { const crd::u32 h = (static_cast<crd::u32>(i) * 2654435761U) ^ 0x9E3779B9U; return (static_cast<float>(h & 0xFFFFU) / 32768.0F) - 1.0F; };
    crd::containers::Array<float> wstar(&alloc); wstar.resize(uz(d_n * d_n));
    crd::containers::Array<float> bstar(&alloc); bstar.resize(uz(d_n));
    for (int i = 0; i < d_n * d_n; ++i) { wstar[uz(i)] = rnd(i) * 0.20F; }
    for (int i = 0; i < d_n; ++i) { bstar[uz(i)] = rnd(i + 555) * 0.10F; }
    crd::containers::Array<crd::u16> x16(&alloc);  x16.resize(uz(n_s * d_n));
    crd::containers::Array<crd::u16> t16(&alloc);  t16.resize(uz(n_s * d_n));
    crd::containers::Array<float>    xf(&alloc);   xf.resize(uz(n_s * d_n));
    for (int i = 0; i < n_s * d_n; ++i) { const float xv = rnd(i + 9001); xf[uz(i)] = crd::math::f16_bits_to_f32(crd::math::f32_to_f16_bits(xv)); x16[uz(i)] = crd::math::f32_to_f16_bits(xv); }
    for (int s = 0; s < n_s; ++s) { for (int m = 0; m < d_n; ++m) { float acc = bstar[uz(m)]; for (int k = 0; k < d_n; ++k) { acc += wstar[uz(m * d_n + k)] * xf[uz(s * d_n + k)]; } t16[uz(s * d_n + m)] = crd::math::f32_to_f16_bits(acc); } }

    kir::GlslKernel kern(&alloc);
    REQUIRE(nn::emit_coopvec_linear_train_glsl(d_n, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "coopvec_train", &alloc);
    if (!spv.ok) { WARN("coopvec train GLSL->SPIR-V failed: " << spv.error_message.c_str()); }
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 8, 0U);
    REQUIRE(pipe != nullptr);

    // the host layout-convert (TrainingOptimal weight-grad → RowMajor) — a device-level extension function.
    auto pfn_conv = reinterpret_cast<PFN_vkConvertCooperativeVectorMatrixNV>(vkGetDeviceProcAddr(vk->vk_device(), "vkConvertCooperativeVectorMatrixNV"));
    REQUIRE(pfn_conv != nullptr);
    // query the TrainingOptimal byte size of a d_n×d_n fp16 matrix (dstData null ⇒ just fill *pDstSize).
    size_t to_size = 0;
    {
        VkConvertCooperativeVectorMatrixInfoNV q{};
        q.sType             = VK_STRUCTURE_TYPE_CONVERT_COOPERATIVE_VECTOR_MATRIX_INFO_NV;
        q.srcSize           = static_cast<size_t>(d_n * d_n) * 2U;
        q.pDstSize          = &to_size;
        q.srcComponentType  = VK_COMPONENT_TYPE_FLOAT16_KHR;
        q.dstComponentType  = VK_COMPONENT_TYPE_FLOAT16_KHR;
        q.numRows           = static_cast<crd::u32>(d_n);
        q.numColumns        = static_cast<crd::u32>(d_n);
        q.srcLayout         = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_ROW_MAJOR_NV;
        q.srcStride         = static_cast<size_t>(d_n) * 2U;
        q.dstLayout         = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_TRAINING_OPTIMAL_NV;
        REQUIRE(pfn_conv(vk->vk_device(), &q) == VK_SUCCESS);
    }
    REQUIRE(to_size > 0U);

    auto d_x   = compute.create_buffer(static_cast<crd::u64>(n_s * d_n) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_t   = compute.create_buffer(static_cast<crd::u64>(n_s * d_n) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_w   = compute.create_buffer(static_cast<crd::u64>(d_n * d_n) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_b   = compute.create_buffer(static_cast<crd::u64>(d_n) * 2U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_gw  = compute.create_buffer(static_cast<crd::u64>(to_size), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_gb  = compute.create_buffer(static_cast<crd::u64>(d_n) * 2U, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_y   = compute.create_buffer(static_cast<crd::u64>(n_s * d_n) * 2U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    auto d_cfg = compute.create_buffer(4U * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    const auto up_bytes = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nbytes) {
        auto  stg = compute.create_buffer(nbytes, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<crd::u8*>(stg->map());
        const auto* srcb = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nbytes; ++i) { p[i] = srcb[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nbytes);
        compute.submit_and_wait();
    };
    const auto read_bytes = [&](cg::ComputeBuffer& src, void* dstp, crd::u64 nbytes) {
        auto rbf = compute.create_buffer(nbytes, transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto& r2 = compute.begin();
        r2.barrier(src, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
        r2.copy(src, *rbf, 0U, 0U, nbytes);
        compute.submit_and_wait();
        const auto* p = static_cast<const crd::u8*>(rbf->map());
        auto*       d = static_cast<crd::u8*>(dstp);
        for (crd::u64 i = 0; i < nbytes; ++i) { d[i] = p[i]; }
        rbf->unmap();
    };
    const crd::u32 cfgv[4] = {static_cast<crd::u32>(n_s), 0U, 0U, 0U};
    up_bytes(*d_x, x16.data(), static_cast<crd::u64>(n_s * d_n) * 2U);
    up_bytes(*d_t, t16.data(), static_cast<crd::u64>(n_s * d_n) * 2U);
    up_bytes(*d_cfg, cfgv, 4U * 4U);

    // train from W=0, b=0.
    crd::containers::Array<float>    wf(&alloc);   wf.resize(uz(d_n * d_n), 0.0F);
    crd::containers::Array<float>    bf(&alloc);   bf.resize(uz(d_n), 0.0F);
    crd::containers::Array<crd::u16> w16(&alloc);  w16.resize(uz(d_n * d_n), static_cast<crd::u16>(0));
    crd::containers::Array<crd::u16> b16(&alloc);  b16.resize(uz(d_n), static_cast<crd::u16>(0));
    crd::containers::Array<crd::u8>  zw(&alloc);   zw.resize(uz(static_cast<int>(to_size)), static_cast<crd::u8>(0));
    crd::containers::Array<crd::u16> zb(&alloc);   zb.resize(uz(d_n), static_cast<crd::u16>(0));
    crd::containers::Array<crd::u8>  gw_to(&alloc); gw_to.resize(uz(static_cast<int>(to_size)));
    crd::containers::Array<crd::u16> gw_rm(&alloc); gw_rm.resize(uz(d_n * d_n));
    crd::containers::Array<crd::u16> gb16(&alloc);  gb16.resize(uz(d_n));
    crd::containers::Array<crd::u16> yout(&alloc);  yout.resize(uz(n_s * d_n));

    const float lr    = 0.1F;
    const int   steps = 150;
    double      loss0 = 0.0;
    double      lossf = 0.0;
    for (int step = 0; step < steps; ++step)
    {
        up_bytes(*d_w, w16.data(), static_cast<crd::u64>(d_n * d_n) * 2U);
        up_bytes(*d_b, b16.data(), static_cast<crd::u64>(d_n) * 2U);
        up_bytes(*d_gw, zw.data(), static_cast<crd::u64>(to_size));   // zero the gradient accumulators
        up_bytes(*d_gb, zb.data(), static_cast<crd::u64>(d_n) * 2U);
        auto& rec = compute.begin();
        cg::ComputeBuffer* binds[8] = {d_x.get(), d_t.get(), d_w.get(), d_b.get(), d_gw.get(), d_gb.get(), d_y.get(), d_cfg.get()};
        rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 8), nullptr, 0U, static_cast<crd::u32>((n_s + 63) / 64), 1U, 1U);
        rec.barrier(*d_y, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        compute.submit_and_wait();

        read_bytes(*d_y, yout.data(), static_cast<crd::u64>(n_s * d_n) * 2U);
        double loss = 0.0;
        for (int i = 0; i < n_s * d_n; ++i) { const float e = crd::math::f16_bits_to_f32(yout[uz(i)]) - crd::math::f16_bits_to_f32(t16[uz(i)]); loss += static_cast<double>(e * e); }
        loss /= static_cast<double>(n_s * d_n);
        if (step == 0) { loss0 = loss; }
        lossf = loss;

        // read the TrainingOptimal weight-grad, convert → RowMajor; read the bias-grad.
        read_bytes(*d_gw, gw_to.data(), static_cast<crd::u64>(to_size));
        read_bytes(*d_gb, gb16.data(), static_cast<crd::u64>(d_n) * 2U);
        size_t rm_size = static_cast<size_t>(d_n * d_n) * 2U;
        VkConvertCooperativeVectorMatrixInfoNV c{};
        c.sType             = VK_STRUCTURE_TYPE_CONVERT_COOPERATIVE_VECTOR_MATRIX_INFO_NV;
        c.srcSize           = to_size;
        c.srcData.hostAddress = gw_to.data();
        c.pDstSize          = &rm_size;
        c.dstData.hostAddress = gw_rm.data();
        c.srcComponentType  = VK_COMPONENT_TYPE_FLOAT16_KHR;
        c.dstComponentType  = VK_COMPONENT_TYPE_FLOAT16_KHR;
        c.numRows           = static_cast<crd::u32>(d_n);
        c.numColumns        = static_cast<crd::u32>(d_n);
        c.srcLayout         = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_TRAINING_OPTIMAL_NV;
        c.dstLayout         = VK_COOPERATIVE_VECTOR_MATRIX_LAYOUT_ROW_MAJOR_NV;
        c.dstStride         = static_cast<size_t>(d_n) * 2U;
        REQUIRE(pfn_conv(vk->vk_device(), &c) == VK_SUCCESS);

        if (step == 0) // the on-device hardware gradient must equal the CPU reference gradient (W=0,b=0 ⇒ y=0 ⇒ δ = -2t)
        {
            crd::containers::Array<float> gw_ref(&alloc); gw_ref.resize(uz(d_n * d_n), 0.0F);
            crd::containers::Array<float> gb_ref(&alloc); gb_ref.resize(uz(d_n), 0.0F);
            for (int s = 0; s < n_s; ++s) { for (int m = 0; m < d_n; ++m) { const float dm = 2.0F * (0.0F - crd::math::f16_bits_to_f32(t16[uz(s * d_n + m)])); gb_ref[uz(m)] += dm; for (int k = 0; k < d_n; ++k) { gw_ref[uz(m * d_n + k)] += dm * crd::math::f16_bits_to_f32(x16[uz(s * d_n + k)]); } } }
            float gwerr = 0.0F;
            for (int i = 0; i < d_n * d_n; ++i) { const float e = crd::math::f16_bits_to_f32(gw_rm[uz(i)]) - gw_ref[uz(i)]; const float a = e < 0.0F ? -e : e; if (a > gwerr) { gwerr = a; } }
            float gberr = 0.0F;
            for (int i = 0; i < d_n; ++i) { const float e = crd::math::f16_bits_to_f32(gb16[uz(i)]) - gb_ref[uz(i)]; const float a = e < 0.0F ? -e : e; if (a > gberr) { gberr = a; } }
            std::printf("[coopvec-train] step0 device gradient vs CPU ref: worst dW err=%.3f, worst db err=%.3f (accum magnitude ~%d)\n", static_cast<double>(gwerr), static_cast<double>(gberr), n_s);
            CHECK(gwerr < 2.0F); // fp16 accumulation over the batch (values ~O(N)); the hardware outer-product matches the reference
            CHECK(gberr < 2.0F);
        }

        // SGD apply (host; the heavy backprop was on the tensor path) + re-quantize for the next step.
        const float inv = lr / static_cast<float>(n_s);
        for (int i = 0; i < d_n * d_n; ++i) { wf[uz(i)] -= inv * crd::math::f16_bits_to_f32(gw_rm[uz(i)]); w16[uz(i)] = crd::math::f32_to_f16_bits(wf[uz(i)]); }
        for (int i = 0; i < d_n; ++i) { bf[uz(i)] -= inv * crd::math::f16_bits_to_f32(gb16[uz(i)]); b16[uz(i)] = crd::math::f32_to_f16_bits(bf[uz(i)]); }
    }
    std::printf("[coopvec-train] linear %dx%d, N=%d, %d steps: loss %.5f -> %.5f (%.1f%% down) -- trained on the tensor path\n", d_n, d_n, n_s, steps, loss0, lossf, 100.0 * (1.0 - lossf / loss0));
    CHECK(lossf < loss0 * 0.05); // the on-device gradients drove the loss to <5% of its start (it learned the map)
}

// D-007 B11: the CKIR WAVE / SUBGROUP op class — reduce (add/max/or) · inclusive/exclusive prefix scan · broadcast-first · shuffle
// — DISPATCHES on Vulkan (GLSL `subgroupAdd`/`subgroupInclusiveAdd`/…) and matches the CPU oracle BIT-EXACT. Integer ops are
// order-independent ⇒ bit-exact/portable (the mission bar). One 64-thread workgroup = two 32-lane subgroups (this NVIDIA HW).
TEST_CASE("D-007 B11: CKIR wave/subgroup ops (reduce/scan/broadcast/shuffle) DISPATCH on Vulkan == oracle bit-exact",
          "[gpu-context][vulkan][gpu][subgroup]")
{
    namespace cg  = crd::gpu;
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(16U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    constexpr int t_n = 64;
    constexpr int no  = crd::gputest::kSubgroupNOut;
    // REN-38 llvmpipe campaign: the kernel AND the oracle are shaped by the DEVICE subgroup width (llvmpipe 8).
    const crd::u32 lanes = compute.subgroup_size();
    if (lanes == 0U || lanes > 32U) { SKIP("no u32-maskable subgroup width reported"); }
    kir::KGraph      g(&alloc);
    const kir::KEntry e = crd::gputest::build_subgroup_ops_kernel(g, t_n, static_cast<int>(lanes));

    crd::containers::Array<crd::u32> xin(&alloc);   xin.resize(uz(t_n));
    for (int i = 0; i < t_n; ++i) { xin[uz(i)] = (static_cast<crd::u32>(i) * 2654435761U) & 0xFFU; } // small ⇒ no u32 wrap
    // CPU ORACLE reference (f64 domain; round_dtype wraps to u32).
    crd::containers::Array<crd::f64> xin64(&alloc); xin64.resize(uz(t_n));
    crd::containers::Array<crd::f64> out64(&alloc); out64.resize(uz(no * t_n), 0.0);
    for (int i = 0; i < t_n; ++i) { xin64[uz(i)] = static_cast<crd::f64>(xin[uz(i)]); }
    kir::KernelBuffer bufs[2] = {{xin64.data(), t_n, 0, 0}, {out64.data(), no * t_n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, static_cast<crd::u32>(t_n), &alloc, 1U, lanes);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "subgroup_ops", &alloc);
    if (!spv.ok) { WARN("subgroup GLSL->SPIR-V failed: " << spv.error_message.c_str()); }
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    auto d_in  = compute.create_buffer(static_cast<crd::u64>(t_n) * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out = compute.create_buffer(static_cast<crd::u64>(no * t_n) * 4U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    {
        auto  stg = compute.create_buffer(static_cast<crd::u64>(t_n) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<crd::u32*>(stg->map());
        for (int i = 0; i < t_n; ++i) { p[i] = xin[uz(i)]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, *d_in, 0U, 0U, static_cast<crd::u64>(t_n) * 4U);
        compute.submit_and_wait();
    }
    auto& rec = compute.begin();
    cg::ComputeBuffer* binds[2] = {d_in.get(), d_out.get()};
    rec.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 2), nullptr, 0U, 1U, 1U, 1U);
    rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(no * t_n) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r2 = compute.begin(); r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(no * t_n) * 4U); compute.submit_and_wait(); }
    const auto* out = static_cast<const crd::u32*>(rb->map());
    int bad = 0;
    for (int i = 0; i < no * t_n; ++i) { if (out[uz(i)] != static_cast<crd::u32>(static_cast<crd::i64>(out64[uz(i)]))) { ++bad; } }
    rb->unmap();
    std::printf("[subgroup] 64-thread wg (2x32-lane): %d/%d results mismatched vs oracle (add/max/incl/excl/bcast/shuffle/or)\n", bad, no * t_n);
    CHECK(bad == 0); // GPU wave ops == CPU oracle, bit-exact
}

// D-007 C5: GPU-DRIVEN DISPATCH — a compute pass COUNTS how many inputs pass a predicate and writes the next pass's workgroup
// count into an INDIRECT-args buffer; `dispatch_indirect` then launches EXACTLY that many groups with NO CPU round-trip (the GPU
// decides its own workload). Verifies the second pass ran the GPU-decided number of groups (== the CPU reference count).
TEST_CASE("D-007 C5: GPU-driven indirect dispatch on Vulkan -- a compute pass decides the next dispatch's size",
          "[gpu-context][vulkan][gpu][indirect]")
{
    namespace cg = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(16U << 20U);
    using cg::compute_usage::indirect;
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    static const char* const kCountSrc =
        "#version 460\n"
        "layout(local_size_x = 1) in;\n"
        "layout(std430, binding=0) readonly  buffer BIn   { uint vals[]; };\n"
        "layout(std430, binding=1) writeonly buffer BArgs { uint args[]; };\n"
        "layout(std430, binding=2) readonly  buffer BCfg  { uint n; };\n"
        "void main() { uint c = 0u; for (uint i = 0u; i < n; ++i) { if ((vals[i] & 1u) == 0u) { c += 1u; } }\n"
        "  args[0] = c; args[1] = 1u; args[2] = 1u; }\n"; // groupCountX = the GPU-computed count
    static const char* const kWorkSrc =
        "#version 460\n"
        "layout(local_size_x = 1) in;\n"
        "layout(std430, binding=0) writeonly buffer BOut { uint o[]; };\n"
        "void main() { o[gl_WorkGroupID.x] = gl_WorkGroupID.x + 1u; }\n"; // one group per unit of GPU-decided work

    const auto mk = [&](const char* src, int nb) {
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::StringView(src), "c5", &alloc);
        REQUIRE(spv.ok);
        return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
    };
    auto pipe_count = mk(kCountSrc, 3);
    auto pipe_work  = mk(kWorkSrc, 1);
    REQUIRE(pipe_count != nullptr);
    REQUIRE(pipe_work != nullptr);

    constexpr int n_v = 256;
    int           ref = 0;
    crd::containers::Array<crd::u32> vals(&alloc); vals.resize(uz(n_v));
    for (int i = 0; i < n_v; ++i) { const crd::u32 v = (static_cast<crd::u32>(i) * 2654435761U) >> 3U; vals[uz(i)] = v; if ((v & 1U) == 0U) { ++ref; } }

    auto d_in   = compute.create_buffer(static_cast<crd::u64>(n_v) * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_args = compute.create_buffer(3U * 4U, storage | indirect | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_cfg  = compute.create_buffer(4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_out  = compute.create_buffer(static_cast<crd::u64>(n_v) * 4U, storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    const auto up = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nb) {
        auto stg = compute.create_buffer(nb, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p = static_cast<crd::u8*>(stg->map()); const auto* s = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nb; ++i) { p[i] = s[i]; } stg->unmap();
        auto& rc = compute.begin(); rc.copy(*stg, dst, 0U, 0U, nb); compute.submit_and_wait();
    };
    const crd::u32 cfgv = static_cast<crd::u32>(n_v);
    const crd::u32 zeros[3] = {0U, 0U, 0U};
    up(*d_in, vals.data(), static_cast<crd::u64>(n_v) * 4U);
    up(*d_cfg, &cfgv, 4U);
    up(*d_args, zeros, 3U * 4U);
    { auto stg = compute.create_buffer(static_cast<crd::u64>(n_v) * 4U, transfer_src, cg::ComputeMemory::CpuToGpu); auto* p = static_cast<crd::u32*>(stg->map()); for (int i = 0; i < n_v; ++i) { p[i] = 0U; } stg->unmap(); auto& rc = compute.begin(); rc.copy(*stg, *d_out, 0U, 0U, static_cast<crd::u64>(n_v) * 4U); compute.submit_and_wait(); }

    auto& rec = compute.begin();
    cg::ComputeBuffer* cb[3] = {d_in.get(), d_args.get(), d_cfg.get()};
    rec.dispatch(*pipe_count, crd::containers::ConstSpan<cg::ComputeBuffer*>(cb, 3), nullptr, 0U, 1U, 1U, 1U); // the GPU counts
    rec.barrier(*d_args, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::IndirectRead);                    // write → indirect fetch
    cg::ComputeBuffer* wb[1] = {d_out.get()};
    rec.dispatch_indirect(*pipe_work, crd::containers::ConstSpan<cg::ComputeBuffer*>(wb, 1), nullptr, 0U, *d_args, 0U); // GPU-decided count
    rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
    compute.submit_and_wait();

    auto rb = compute.create_buffer(static_cast<crd::u64>(n_v) * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    { auto& r2 = compute.begin(); r2.copy(*d_out, *rb, 0U, 0U, static_cast<crd::u64>(n_v) * 4U); compute.submit_and_wait(); }
    const auto* out = static_cast<const crd::u32*>(rb->map());
    int written = 0;
    for (int i = 0; i < n_v; ++i) { if (out[uz(i)] != 0U) { ++written; } }
    rb->unmap();
    std::printf("[c5-indirect] GPU counted %d even inputs -> dispatch_indirect launched %d groups (CPU ref = %d)\n", written, written, ref);
    CHECK(written == ref); // the GPU-decided dispatch launched EXACTLY the reference count of groups
}

// D-007 C5 (frontier): DEVICE-GENERATED COMMANDS — the GPU executes a generated STREAM of VARIED commands in ONE call
// (VK_NV_device_generated_commands). Each sequence carries its OWN push constants + its own dispatch (and, in the pipeline-switch
// variant, its own pipeline) — the GPU authors its own command buffer, the Nanite-class primitive beyond a bare indirect count.
// Raw Vulkan on the converged device. Verifies each generated sequence ran with its own state.
TEST_CASE("D-007 C5: device-generated commands -- a generated stream of VARIED compute commands (push + dispatch per sequence)",
          "[gpu-context][vulkan][gpu][dgc]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->device_generated_commands()) { WARN("no VK_NV_device_generated_commands; skipping"); return; }
    const VkDevice         dev  = vk->vk_device();
    const VkPhysicalDevice phys = vk->vk_physical_device();
    const VkQueue          q    = vk->compute_queue();
    const crd::u32         qf   = vk->compute_family();
    crd::memory::TlsfAllocator alloc(8U << 20U);

    // ── DGC proc addresses (device extension functions) ──
    auto pfn_create = reinterpret_cast<PFN_vkCreateIndirectCommandsLayoutNV>(vkGetDeviceProcAddr(dev, "vkCreateIndirectCommandsLayoutNV"));
    auto pfn_destroy = reinterpret_cast<PFN_vkDestroyIndirectCommandsLayoutNV>(vkGetDeviceProcAddr(dev, "vkDestroyIndirectCommandsLayoutNV"));
    auto pfn_memreq = reinterpret_cast<PFN_vkGetGeneratedCommandsMemoryRequirementsNV>(vkGetDeviceProcAddr(dev, "vkGetGeneratedCommandsMemoryRequirementsNV"));
    auto pfn_exec = reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsNV>(vkGetDeviceProcAddr(dev, "vkCmdExecuteGeneratedCommandsNV"));
    REQUIRE(pfn_create != nullptr);
    REQUIRE(pfn_memreq != nullptr);
    REQUIRE(pfn_exec != nullptr);

    // ── a tiny raw-Vulkan buffer allocator (host-visible|coherent; optional device address) ──
    VkPhysicalDeviceMemoryProperties memprops{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    const auto find_mem = [&](crd::u32 type_bits, VkMemoryPropertyFlags want) -> crd::u32 {
        for (crd::u32 i = 0; i < memprops.memoryTypeCount; ++i) { if ((type_bits & (1U << i)) != 0U && (memprops.memoryTypes[i].propertyFlags & want) == want) { return i; } }
        return 0U;
    };
    struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; };
    const auto make_buf = [&](VkDeviceSize size, VkBufferUsageFlags usage, bool device_addr) -> Buf {
        Buf b{};
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size  = size;
        bci.usage = usage | (device_addr ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) : 0U);
        vkCreateBuffer(dev, &bci, nullptr, &b.buf);
        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(dev, b.buf, &mr);
        VkMemoryAllocateFlagsInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext           = device_addr ? &fi : nullptr;
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &b.mem);
        vkBindBufferMemory(dev, b.buf, b.mem, 0);
        if (device_addr) { VkBufferDeviceAddressInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO; ai.buffer = b.buf; b.addr = vkGetBufferDeviceAddress(dev, &ai); }
        return b;
    };

    constexpr int n_seq = 4;
    constexpr int out_n = 16;
    Buf d_out = make_buf(static_cast<VkDeviceSize>(out_n) * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    { void* p = nullptr; vkMapMemory(dev, d_out.mem, 0, VK_WHOLE_SIZE, 0, &p); for (int i = 0; i < out_n; ++i) { static_cast<crd::u32*>(p)[i] = 0U; } vkUnmapMemory(dev, d_out.mem); }

    // ── descriptor set layout (1 storage buffer) + pipeline layout (+ an 8-byte push constant {slot,val}) ──
    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding = 0; dslb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; dslb.descriptorCount = 1; dslb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{}; dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dslci.bindingCount = 1; dslci.pBindings = &dslb;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl);
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.offset = 0; pcr.size = 8;
    VkPipelineLayoutCreateInfo plci{}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; plci.setLayoutCount = 1; plci.pSetLayouts = &dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout playout = VK_NULL_HANDLE; vkCreatePipelineLayout(dev, &plci, nullptr, &playout);

    // ── the compute pipeline: o[slot] = val (both from the per-sequence push constant) ──
    static const char* const kSrc =
        "#version 460\n"
        "layout(local_size_x = 1) in;\n"
        "layout(std430, binding = 0) buffer Out { uint o[]; };\n"
        "layout(push_constant) uniform P { uint slot; uint val; };\n"
        "void main() { o[slot] = val; }\n";
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::StringView(kSrc), "dgc", &alloc);
    REQUIRE(spv.ok);
    VkShaderModuleCreateInfo smci{}; smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; smci.codeSize = spv.spirv.size(); smci.pCode = reinterpret_cast<const crd::u32*>(spv.spirv.data());
    VkShaderModule sm = VK_NULL_HANDLE; vkCreateShaderModule(dev, &smci, nullptr, &sm);
    VkComputePipelineCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cpci.layout = playout;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm; cpci.stage.pName = "main";
    VkPipeline pipe = VK_NULL_HANDLE; vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
    REQUIRE(pipe != VK_NULL_HANDLE);

    // ── descriptor pool + set (binding 0 = the output buffer) ──
    VkDescriptorPoolSize dps{}; dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; dps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{}; dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
    VkDescriptorPool pool = VK_NULL_HANDLE; vkCreateDescriptorPool(dev, &dpci, nullptr, &pool);
    VkDescriptorSetAllocateInfo dsai{}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet set = VK_NULL_HANDLE; vkAllocateDescriptorSets(dev, &dsai, &set);
    VkDescriptorBufferInfo dbi{}; dbi.buffer = d_out.buf; dbi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet wr{}; wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = set; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);

    // ── the INDIRECT COMMANDS LAYOUT: [PUSH_CONSTANT @0 (8B), DISPATCH @8 (12B)] per sequence, stride 20, bind point COMPUTE ──
    VkIndirectCommandsLayoutTokenNV toks[2]{};
    toks[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_NV; toks[0].tokenType = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_NV;
    toks[0].stream = 0; toks[0].offset = 0; toks[0].pushconstantPipelineLayout = playout; toks[0].pushconstantShaderStageFlags = VK_SHADER_STAGE_COMPUTE_BIT; toks[0].pushconstantOffset = 0; toks[0].pushconstantSize = 8;
    toks[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_NV; toks[1].tokenType = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_NV; toks[1].stream = 0; toks[1].offset = 8;
    const crd::u32 stride = 20U;
    VkIndirectCommandsLayoutCreateInfoNV lci{}; lci.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_NV; lci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; lci.tokenCount = 2; lci.pTokens = toks; lci.streamCount = 1; lci.pStreamStrides = &stride;
    VkIndirectCommandsLayoutNV iclayout = VK_NULL_HANDLE;
    REQUIRE(pfn_create(dev, &lci, nullptr, &iclayout) == VK_SUCCESS);

    // ── the INPUT STREAM: n_seq sequences, each {slot u32, val u32, dispatch {x,y,z}} = 20 bytes. Varied per sequence. ──
    Buf d_stream = make_buf(static_cast<VkDeviceSize>(n_seq) * stride, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    const crd::u32 slots[n_seq] = {2U, 5U, 9U, 13U};
    const crd::u32 vals[n_seq]  = {111U, 222U, 333U, 444U};
    { auto* p = static_cast<crd::u8*>(nullptr); vkMapMemory(dev, d_stream.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&p));
      for (int s = 0; s < n_seq; ++s) { auto* u = reinterpret_cast<crd::u32*>(p + static_cast<crd::usize>(s) * stride); u[0] = slots[s]; u[1] = vals[s]; u[2] = 1U; u[3] = 1U; u[4] = 1U; }
      vkUnmapMemory(dev, d_stream.mem); }

    // ── preprocess buffer (from the generated-commands memory requirements) ──
    VkGeneratedCommandsMemoryRequirementsInfoNV gmri{}; gmri.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_NV; gmri.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; gmri.pipeline = pipe; gmri.indirectCommandsLayout = iclayout; gmri.maxSequencesCount = n_seq;
    VkMemoryRequirements2 gmr{}; gmr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
    pfn_memreq(dev, &gmri, &gmr);
    Buf d_pre = make_buf(gmr.memoryRequirements.size > 0 ? gmr.memoryRequirements.size : 4U, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);

    // ── record: bind pipeline + descriptor set (state the generated commands inherit), then EXECUTE the generated stream ──
    VkCommandPoolCreateInfo cpci2{}; cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpci2.queueFamilyIndex = qf;
    VkCommandPool cpool = VK_NULL_HANDLE; vkCreateCommandPool(dev, &cpci2, nullptr, &cpool);
    VkCommandBufferAllocateInfo cbai{}; cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE; vkAllocateCommandBuffers(dev, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi{}; cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, nullptr);
    VkIndirectCommandsStreamNV strm{}; strm.buffer = d_stream.buf; strm.offset = 0;
    VkGeneratedCommandsInfoNV gci{}; gci.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_NV; gci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; gci.pipeline = pipe; gci.indirectCommandsLayout = iclayout; gci.streamCount = 1; gci.pStreams = &strm; gci.sequencesCount = n_seq; gci.preprocessBuffer = d_pre.buf; gci.preprocessSize = gmr.memoryRequirements.size;
    pfn_exec(cmd, VK_FALSE, &gci);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);

    // ── verify: each generated sequence wrote its OWN slot ← its OWN val (varied push constant + dispatch, one execute call) ──
    int good = 0;
    { void* p = nullptr; vkMapMemory(dev, d_out.mem, 0, VK_WHOLE_SIZE, 0, &p); const auto* o = static_cast<const crd::u32*>(p);
      for (int s = 0; s < n_seq; ++s) { if (o[slots[s]] == vals[s]) { ++good; } }
      vkUnmapMemory(dev, d_out.mem); }
    std::printf("[dgc] device-generated stream: %d/%d sequences ran with their own {slot,val,dispatch} via ONE execute call\n", good, n_seq);
    CHECK(good == n_seq);

    pfn_destroy(dev, iclayout, nullptr);
    vkDestroyCommandPool(dev, cpool, nullptr);
    vkDestroyDescriptorPool(dev, pool, nullptr);
    vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyShaderModule(dev, sm, nullptr);
    vkDestroyPipelineLayout(dev, playout, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    for (Buf* b : {&d_out, &d_stream, &d_pre}) { vkDestroyBuffer(dev, b->buf, nullptr); vkFreeMemory(dev, b->mem, nullptr); }
}

// D-007 C5 (frontier, FULL): DEVICE-GENERATED COMMANDS with PER-SEQUENCE PIPELINE SWITCHING — the GPU-authored stream selects a
// DIFFERENT compute pipeline per sequence (the PIPELINE token + indirect-bindable pipelines: metadata buffer → device address).
// Two pipelines (A writes 100+val, B writes 200+val); alternating sequences prove the pipeline switch came from the stream.
TEST_CASE("D-007 C5: device-generated commands -- per-sequence PIPELINE switch (indirect-bindable pipelines)",
          "[gpu-context][vulkan][gpu][dgc]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->device_generated_commands()) { WARN("no VK_NV_device_generated_commands; skipping"); return; }
    const VkDevice         dev  = vk->vk_device();
    const VkPhysicalDevice phys = vk->vk_physical_device();
    const VkQueue          q    = vk->compute_queue();
    const crd::u32         qf   = vk->compute_family();
    crd::memory::TlsfAllocator alloc(8U << 20U);

    auto pfn_create   = reinterpret_cast<PFN_vkCreateIndirectCommandsLayoutNV>(vkGetDeviceProcAddr(dev, "vkCreateIndirectCommandsLayoutNV"));
    auto pfn_destroy  = reinterpret_cast<PFN_vkDestroyIndirectCommandsLayoutNV>(vkGetDeviceProcAddr(dev, "vkDestroyIndirectCommandsLayoutNV"));
    auto pfn_memreq   = reinterpret_cast<PFN_vkGetGeneratedCommandsMemoryRequirementsNV>(vkGetDeviceProcAddr(dev, "vkGetGeneratedCommandsMemoryRequirementsNV"));
    auto pfn_exec     = reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsNV>(vkGetDeviceProcAddr(dev, "vkCmdExecuteGeneratedCommandsNV"));
    auto pfn_pmemreq  = reinterpret_cast<PFN_vkGetPipelineIndirectMemoryRequirementsNV>(vkGetDeviceProcAddr(dev, "vkGetPipelineIndirectMemoryRequirementsNV"));
    auto pfn_pupdate  = reinterpret_cast<PFN_vkCmdUpdatePipelineIndirectBufferNV>(vkGetDeviceProcAddr(dev, "vkCmdUpdatePipelineIndirectBufferNV"));
    auto pfn_paddr    = reinterpret_cast<PFN_vkGetPipelineIndirectDeviceAddressNV>(vkGetDeviceProcAddr(dev, "vkGetPipelineIndirectDeviceAddressNV"));
    REQUIRE(pfn_create != nullptr); REQUIRE(pfn_pmemreq != nullptr); REQUIRE(pfn_pupdate != nullptr); REQUIRE(pfn_paddr != nullptr);

    VkPhysicalDeviceMemoryProperties memprops{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    const auto find_mem = [&](crd::u32 tb, VkMemoryPropertyFlags want) -> crd::u32 { for (crd::u32 i = 0; i < memprops.memoryTypeCount; ++i) { if ((tb & (1U << i)) != 0U && (memprops.memoryTypes[i].propertyFlags & want) == want) { return i; } } return 0U; };
    struct Buf { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceAddress addr = 0; };
    const auto make_buf = [&](VkDeviceSize size, VkBufferUsageFlags usage, bool da) -> Buf {
        Buf b{};
        VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = size; bci.usage = usage | (da ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) : 0U);
        vkCreateBuffer(dev, &bci, nullptr, &b.buf);
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(dev, b.buf, &mr);
        VkMemoryAllocateFlagsInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO; fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.pNext = da ? &fi : nullptr; mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(dev, &mai, nullptr, &b.mem); vkBindBufferMemory(dev, b.buf, b.mem, 0);
        if (da) { VkBufferDeviceAddressInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO; ai.buffer = b.buf; b.addr = vkGetBufferDeviceAddress(dev, &ai); }
        return b;
    };

    constexpr int out_n = 16;
    Buf d_out = make_buf(static_cast<VkDeviceSize>(out_n) * 4U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    { void* p = nullptr; vkMapMemory(dev, d_out.mem, 0, VK_WHOLE_SIZE, 0, &p); for (int i = 0; i < out_n; ++i) { static_cast<crd::u32*>(p)[i] = 0U; } vkUnmapMemory(dev, d_out.mem); }

    VkDescriptorSetLayoutBinding dslb{}; dslb.binding = 0; dslb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; dslb.descriptorCount = 1; dslb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{}; dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dslci.bindingCount = 1; dslci.pBindings = &dslb;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &dsl);
    VkPushConstantRange pcr{}; pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; pcr.offset = 0; pcr.size = 8;
    VkPipelineLayoutCreateInfo plci{}; plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; plci.setLayoutCount = 1; plci.pSetLayouts = &dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    VkPipelineLayout playout = VK_NULL_HANDLE; vkCreatePipelineLayout(dev, &plci, nullptr, &playout);

    // build an INDIRECT-BINDABLE compute pipeline whose shader writes o[slot] = base + val; returns {pipe, module, metadata buf, device addr}.
    struct IndPipe { VkPipeline pipe = VK_NULL_HANDLE; VkShaderModule sm = VK_NULL_HANDLE; Buf meta{}; VkDeviceAddress addr = 0; };
    const auto build_pipe = [&](crd::u32 base) -> IndPipe {
        IndPipe r{};
        char src[256];
        std::snprintf(src, sizeof(src), "#version 460\nlayout(local_size_x=1) in;\nlayout(std430,binding=0) buffer O{uint o[];};\nlayout(push_constant) uniform P{uint slot;uint val;};\nvoid main(){o[slot]=%uu+val;}\n", base);
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::StringView(src), "dgcp", &alloc);
        REQUIRE(spv.ok);
        VkShaderModuleCreateInfo smci{}; smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; smci.codeSize = spv.spirv.size(); smci.pCode = reinterpret_cast<const crd::u32*>(spv.spirv.data());
        vkCreateShaderModule(dev, &smci, nullptr, &r.sm);
        VkPipelineCreateFlags2CreateInfo f2{}; f2.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO; f2.flags = VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_NV;
        VkComputePipelineCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cpci.pNext = &f2; cpci.layout = playout;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = r.sm; cpci.stage.pName = "main";
        VkMemoryRequirements2 mr2{}; mr2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        pfn_pmemreq(dev, &cpci, &mr2); // the pipeline's on-device metadata size
        r.meta = make_buf(mr2.memoryRequirements.size > 0 ? mr2.memoryRequirements.size : 256U, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        VkComputePipelineIndirectBufferInfoNV ib{}; ib.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_INDIRECT_BUFFER_INFO_NV; ib.deviceAddress = r.meta.addr; ib.size = mr2.memoryRequirements.size;
        f2.pNext = &ib; // chain: cpci -> flags2 -> indirect-buffer-info
        REQUIRE(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &r.pipe) == VK_SUCCESS);
        return r;
    };
    IndPipe pa = build_pipe(100U);
    IndPipe pb = build_pipe(200U);

    VkCommandPoolCreateInfo cpci2{}; cpci2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpci2.queueFamilyIndex = qf; cpci2.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool cpool = VK_NULL_HANDLE; vkCreateCommandPool(dev, &cpci2, nullptr, &cpool);
    const auto one_shot = [&](auto&& record) {
        VkCommandBufferAllocateInfo cbai{}; cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer c = VK_NULL_HANDLE; vkAllocateCommandBuffers(dev, &cbai, &c);
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(c, &bi); record(c); vkEndCommandBuffer(c);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &c; vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(q);
        vkFreeCommandBuffers(dev, cpool, 1, &c);
    };
    // populate each pipeline's on-device metadata buffer (required before its device address is usable in the stream)
    one_shot([&](VkCommandBuffer c) { pfn_pupdate(c, VK_PIPELINE_BIND_POINT_COMPUTE, pa.pipe); pfn_pupdate(c, VK_PIPELINE_BIND_POINT_COMPUTE, pb.pipe); });
    { VkPipelineIndirectDeviceAddressInfoNV ai{}; ai.sType = VK_STRUCTURE_TYPE_PIPELINE_INDIRECT_DEVICE_ADDRESS_INFO_NV; ai.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; ai.pipeline = pa.pipe; pa.addr = pfn_paddr(dev, &ai); ai.pipeline = pb.pipe; pb.addr = pfn_paddr(dev, &ai); }

    // descriptor pool + set
    VkDescriptorPoolSize dps{}; dps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; dps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{}; dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
    VkDescriptorPool pool = VK_NULL_HANDLE; vkCreateDescriptorPool(dev, &dpci, nullptr, &pool);
    VkDescriptorSetAllocateInfo dsai{}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsai.descriptorPool = pool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
    VkDescriptorSet set = VK_NULL_HANDLE; vkAllocateDescriptorSets(dev, &dsai, &set);
    VkDescriptorBufferInfo dbi{}; dbi.buffer = d_out.buf; dbi.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet wr{}; wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr.dstSet = set; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo = &dbi;
    vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);

    // layout: [PIPELINE @0 (8B addr), PUSH_CONSTANT @8 (8B), DISPATCH @16 (12B)], stride 32
    VkIndirectCommandsLayoutTokenNV toks[3]{};
    toks[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_NV; toks[0].tokenType = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PIPELINE_NV; toks[0].stream = 0; toks[0].offset = 0;
    toks[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_NV; toks[1].tokenType = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_NV; toks[1].stream = 0; toks[1].offset = 8; toks[1].pushconstantPipelineLayout = playout; toks[1].pushconstantShaderStageFlags = VK_SHADER_STAGE_COMPUTE_BIT; toks[1].pushconstantOffset = 0; toks[1].pushconstantSize = 8;
    toks[2].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_NV; toks[2].tokenType = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_NV; toks[2].stream = 0; toks[2].offset = 16;
    const crd::u32 stride = 32U;
    VkIndirectCommandsLayoutCreateInfoNV lci{}; lci.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_NV; lci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; lci.tokenCount = 3; lci.pTokens = toks; lci.streamCount = 1; lci.pStreamStrides = &stride;
    VkIndirectCommandsLayoutNV iclayout = VK_NULL_HANDLE;
    REQUIRE(pfn_create(dev, &lci, nullptr, &iclayout) == VK_SUCCESS);

    constexpr int n_seq = 4;
    Buf d_stream = make_buf(static_cast<VkDeviceSize>(n_seq) * stride, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
    const VkDeviceAddress pipe_addr[n_seq] = {pa.addr, pb.addr, pa.addr, pb.addr};
    const crd::u32        base_exp[n_seq]  = {100U, 200U, 100U, 200U};
    const crd::u32        slot[n_seq]      = {2U, 5U, 9U, 13U};
    const crd::u32        val[n_seq]       = {1U, 2U, 3U, 4U};
    { crd::u8* p = nullptr; vkMapMemory(dev, d_stream.mem, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&p));
      for (int s = 0; s < n_seq; ++s) { auto* q8 = p + static_cast<crd::usize>(s) * stride; std::memcpy(q8, &pipe_addr[s], 8); auto* u = reinterpret_cast<crd::u32*>(q8 + 8); u[0] = slot[s]; u[1] = val[s]; u[2] = 1U; u[3] = 1U; u[4] = 1U; }
      vkUnmapMemory(dev, d_stream.mem); }

    VkGeneratedCommandsMemoryRequirementsInfoNV gmri{}; gmri.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_NV; gmri.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; gmri.pipeline = pa.pipe; gmri.indirectCommandsLayout = iclayout; gmri.maxSequencesCount = n_seq;
    VkMemoryRequirements2 gmr{}; gmr.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2; pfn_memreq(dev, &gmri, &gmr);
    Buf d_pre = make_buf(gmr.memoryRequirements.size > 0 ? gmr.memoryRequirements.size : 4U, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);

    one_shot([&](VkCommandBuffer c) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, pa.pipe);
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, nullptr);
        VkIndirectCommandsStreamNV strm{}; strm.buffer = d_stream.buf; strm.offset = 0;
        VkGeneratedCommandsInfoNV gci{}; gci.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_NV; gci.pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE; gci.pipeline = pa.pipe; gci.indirectCommandsLayout = iclayout; gci.streamCount = 1; gci.pStreams = &strm; gci.sequencesCount = n_seq; gci.preprocessBuffer = d_pre.buf; gci.preprocessSize = gmr.memoryRequirements.size;
        pfn_exec(c, VK_FALSE, &gci);
    });

    int good = 0;
    { void* p = nullptr; vkMapMemory(dev, d_out.mem, 0, VK_WHOLE_SIZE, 0, &p); const auto* o = static_cast<const crd::u32*>(p);
      for (int s = 0; s < n_seq; ++s) { if (o[slot[s]] == base_exp[s] + val[s]) { ++good; } }
      vkUnmapMemory(dev, d_out.mem); }
    std::printf("[dgc-pipe] GPU-authored stream switched pipelines per sequence: %d/%d (A=100+, B=200+ selected by the PIPELINE token)\n", good, n_seq);
    CHECK(good == n_seq);

    pfn_destroy(dev, iclayout, nullptr);
    vkDestroyCommandPool(dev, cpool, nullptr);
    vkDestroyDescriptorPool(dev, pool, nullptr);
    for (IndPipe* ip : {&pa, &pb}) { vkDestroyPipeline(dev, ip->pipe, nullptr); vkDestroyShaderModule(dev, ip->sm, nullptr); vkDestroyBuffer(dev, ip->meta.buf, nullptr); vkFreeMemory(dev, ip->meta.mem, nullptr); }
    vkDestroyPipelineLayout(dev, playout, nullptr);
    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    for (Buf* b : {&d_out, &d_stream, &d_pre}) { vkDestroyBuffer(dev, b->buf, nullptr); vkFreeMemory(dev, b->mem, nullptr); }
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

// D-007 B4: emit_mesh_hlsl (the DX12 / Shader-Model-6.5 mesh emitter) produces valid HLSL — DXC lowers it to SPIR-V. Proves the
// mesh IR lowers to the DX12 backend too (the device-side mesh PSO + DispatchMesh is the DX12 render follow-up; the Vulkan mesh
// path already renders end-to-end). Covers the triangle AND the real bindless ocean meshlet.
TEST_CASE("D-007 B4: emit_mesh_hlsl MESH shader compiles to SPIR-V via DXC (the DX12 mirror)",
          "[gpu-context][vulkan][gpu][program][raster][hlsl][mesh]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(8U << 20U);
    const auto probe = crd::gpu::compile_hlsl_to_spirv(
        crd::gpu::ShaderStage::Vertex,
        crd::containers::StringView("float4 main() : SV_Position { return float4(0,0,0,1); }"), "b4_probe", &alloc);
    if (!probe.ok)
    {
        WARN("dxc unavailable; skipping B4 mesh HLSL gate");
        return;
    }

    // (1) the mesh TRIANGLE — the SAME graph the Vulkan mesh render draws.
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_triangle_mesh(mg, me);
    kir::GlslKernel mk(&alloc);
    REQUIRE(kir::emit_mesh_hlsl(mg, me, &alloc, mk));
    const auto mspv = crd::gpu::compile_hlsl_to_spirv(crd::gpu::ShaderStage::Mesh, crd::containers::to_view(mk.source), "b4_tri_ms", &alloc);
    INFO(crd::containers::String(mspv.error_message).c_str());
    REQUIRE(mspv.ok);
    CHECK(mspv.spirv.size() >= 4U);

    // (2) the OCEAN meshlet — bindless SampleIndexedLod displacement + a world-position interpolant, the real fast path.
    crd::gputest::OceanCascadeRender ocr;
    kir::KGraph og(&alloc);
    kir::KEntry oe;
    crd::gputest::build_ocean_displaced_mesh(og, oe, 32, 8, ocr);
    kir::GlslKernel ok(&alloc);
    REQUIRE(kir::emit_mesh_hlsl(og, oe, &alloc, ok));
    const auto ospv = crd::gpu::compile_hlsl_to_spirv(crd::gpu::ShaderStage::Mesh, crd::containers::to_view(ok.source), "b4_ocean_ms", &alloc);
    INFO(crd::containers::String(ospv.error_message).c_str());
    REQUIRE(ospv.ok);
    CHECK(ospv.spirv.size() >= 4U);
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

// D-007 B17-a: WEIGHTED-BLENDED ORDER-INDEPENDENT TRANSPARENCY (McGuire-Bavoil 2013) on Vulkan. Four translucent full-screen
// quads accumulate in ONE order-independent pass (RGBA16F additive accum + R16F multiplicative revealage) → a full-screen
// composite resolves them over an opaque background. The whole frame is uniform (every quad is full-screen), so every texel
// must equal the CPU oracle (f16-accum + f32-divide model) within a tight LSB tolerance — a real device WBOIT that matches
// the reference math, the cheap OIT tier. Pairs with the identical DX12 test (one IR, both backends).
TEST_CASE("D-007 B17-a: IR-authored WBOIT draws on Vulkan (accum/reveal MRT + blend + composite -> pixels)",
          "[gpu-context][vulkan][gpu][raster][oit]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("adapter has no VK_EXT_shader_object; skipping"); return; }

    crd::memory::TlsfAllocator alloc(8U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    crd::gputest::WboitScene scene;
    scene.count         = 4U;
    scene.background[0]  = 0.10F; scene.background[1] = 0.10F; scene.background[2] = 0.12F;
    scene.color[0][0] = 0.90F; scene.color[0][1] = 0.15F; scene.color[0][2] = 0.10F; scene.alpha[0] = 0.50F; scene.depth[0] = 0.20F;
    scene.color[1][0] = 0.15F; scene.color[1][1] = 0.85F; scene.color[1][2] = 0.20F; scene.alpha[1] = 0.40F; scene.depth[1] = 0.55F;
    scene.color[2][0] = 0.20F; scene.color[2][1] = 0.25F; scene.color[2][2] = 0.90F; scene.alpha[2] = 0.30F; scene.depth[2] = 0.80F;
    scene.color[3][0] = 0.90F; scene.color[3][1] = 0.85F; scene.color[3][2] = 0.10F; scene.alpha[3] = 0.60F; scene.depth[3] = 0.35F;

    kir::KGraph tvg(&alloc); kir::KEntry tve; crd::gputest::build_wboit_transparent_vs(tvg, tve, scene);
    kir::KGraph tfg(&alloc); kir::KEntry tfe; crd::gputest::build_wboit_transparent_fs(tfg, tfe);
    kir::KGraph cvg(&alloc); kir::KEntry cve; crd::gputest::build_wboit_composite_vs(cvg, cve);
    kir::KGraph cfg2(&alloc); kir::KEntry cfe; crd::gputest::build_wboit_composite_fs(cfg2, cfe);

    auto tvp = ctx->create_program(tvg, tve);
    auto tfp = ctx->create_program(tfg, tfe);
    auto cvp = ctx->create_program(cvg, cve);
    auto cfp = ctx->create_program(cfg2, cfe);
    REQUIRE(tvp != nullptr); REQUIRE(tfp != nullptr); REQUIRE(cvp != nullptr); REQUIRE(cfp != nullptr);

    auto transparent = raster->create_raster_program(*tvp, *tfp);
    auto composite   = raster->create_raster_program(*cvp, *cfp);
    REQUIRE(transparent != nullptr); REQUIRE(transparent->valid());
    REQUIRE(composite != nullptr);   REQUIRE(composite->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    raster->draw_wboit(*target, *transparent, *composite,
                       gpu::ClearColor{scene.background[0], scene.background[1], scene.background[2], 1.0F},
                       scene.count * 6U);

    const crd::u32 expect = crd::gputest::wboit_oracle_pixel(scene);
    if (target->read_pixel(dim / 2U, dim / 2U) == 0U && expect != 0U)
    {
        WARN("draw_wboit produced no output (per-attachment blend equations unavailable?); skipping");
        return;
    }
    crd::u32 worst = 0U;
    for (crd::u32 y = 0U; y < dim; ++y)
    {
        for (crd::u32 x = 0U; x < dim; ++x)
        {
            const crd::u32 d = crd::gputest::rgba8_max_channel_diff(target->read_pixel(x, y), expect);
            if (d > worst) { worst = d; }
        }
    }
    INFO("WBOIT worst per-channel LSB diff vs oracle = " << worst << " (expect 0x" << std::hex << expect << ")");
    CHECK(worst <= 2U);              // f16 accum + f32 divide + blend rounding
    CHECK((expect & 0xFFU) > 0x30U); // sanity: red channel is substantial (transparency actually composited, not just the clear)
}

// D-007 B17-c: the EXACT-REFERENCE A-buffer OIT on Vulkan — deferred per-fragment store + a per-pixel depth SORT + the exact
// front-to-back `over` composite, authored as two portable CKIR compute kernels. The composite is pure f32 mul/add/sub on a
// deterministic (sorted) order, so the device output is BIT-EXACT vs `eval_cpu_kernel` (and DX12 == Vulkan). This exact
// image is the GROUND TRUTH the approximate WBOIT/MBOIT tiers are measured against.
TEST_CASE("D-007 B17-c: exact-reference A-buffer OIT on Vulkan (deferred store + per-pixel sort + composite, bit-exact)",
          "[gpu-context][vulkan][gpu][oit][compute]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg2;
    cfg2.backend  = gpu::GpuBackend::Vulkan;
    cfg2.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(cfg2);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator   alloc(16U << 20U);
    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 4U;
    acfg.local_size = 64U;
    const auto scene = crd::gputest::make_oit_scene();
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_glsl(gr, en, &alloc, kern)) { emit_ok = false; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "ckir_abuffer", &alloc);
        if (!spv.ok) { emit_ok = false; }
        return compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> cpu(&alloc);
    crd::containers::Array<float>    gpu_out(&alloc);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, cpu);
    crd::kir_test::abuffer_dispatch(compute, make_pipe, acfg, scene, alloc, gpu_out);
    REQUIRE(emit_ok);
    REQUIRE(gpu_out.size() == cpu.size());

    double worst = 0.0;
    for (crd::usize i = 0; i < gpu_out.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(gpu_out[i]) - cpu[i]);
        if (d > worst) { worst = d; }
    }
    INFO("A-buffer exact-composite worst |GPU - oracle| = " << worst);
    CHECK(worst == 0.0); // pure f32 mul/add/sub on a deterministic sorted order ⇒ BIT-EXACT
    CHECK(std::fabs(cpu[0] - static_cast<double>(scene.background[0])) > 0.05); // transparency actually composited
}

// D-007 B17-b: MOMENT-BASED OIT (Münstermann 2018 — the hero glass/foliage quality tier) on Vulkan. Per-pixel 4-power-moment
// reconstruction of each fragment's transmittance via the SAME Peters-Klein Hamburger solve as moment shadow maps
// (transcribed scalar in `oit::msm_hamburger_scalar`), from the shared deferred fragment store. Division + ln/exp ⇒ a to-ULP
// tier — GPU matches `eval_cpu_kernel` to a few ULP (both backends). Because 4 power moments represent up to 2 depth masses
// EXACTLY, MBOIT is BIT-EXACT at 2-layer depth complexity (glass front+back, thin foliage) — capturing the exact depth
// ordering the crude single-weight WBOIT tier cannot (30 LSB off the same scene). Higher depth complexity scales with the
// moment count (6/8-moment extension) — the standard MBOIT scaling.
TEST_CASE("D-007 B17-b: moment-based OIT (MBOIT) on Vulkan (4-power-moment reconstruction -- exact depth ordering, beats WBOIT)",
          "[gpu-context][vulkan][gpu][oit][compute]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg2;
    cfg2.backend  = gpu::GpuBackend::Vulkan;
    cfg2.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(cfg2);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator   alloc(32U << 20U);
    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 2U; // 4 power moments represent 2 depth masses EXACTLY — the regime MBOIT resolves precisely (glass)
    acfg.local_size = 64U;
    crd::gputest::WboitScene scene{};
    scene.count = 2U;
    scene.background[0] = 0.05F; scene.background[1] = 0.06F; scene.background[2] = 0.08F;
    scene.color[0][0] = 0.90F; scene.color[0][1] = 0.15F; scene.color[0][2] = 0.10F; scene.alpha[0] = 0.50F; scene.depth[0] = 0.25F;
    scene.color[1][0] = 0.10F; scene.color[1][1] = 0.20F; scene.color[1][2] = 0.90F; scene.alpha[1] = 0.50F; scene.depth[1] = 0.70F;
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_glsl(gr, en, &alloc, kern)) { emit_ok = false; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "ckir_mboit", &alloc);
        if (!spv.ok) { emit_ok = false; }
        return compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> mb_cpu(&alloc);
    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    mb_gpu(&alloc);
    crd::kir_test::mboit_oracle(acfg, scene, alloc, mb_cpu);
    crd::kir_test::mboit_dispatch(compute, make_pipe, acfg, scene, alloc, mb_gpu);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu); // the exact ground truth
    REQUIRE(emit_ok);
    REQUIRE(mb_gpu.size() == mb_cpu.size());

    double worst_ulp = 0.0;
    for (crd::usize i = 0; i < mb_gpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(mb_gpu[i]) - mb_cpu[i]);
        if (d > worst_ulp) { worst_ulp = d; }
    }
    INFO("MBOIT GPU vs oracle worst |Δ| = " << worst_ulp);
    CHECK(worst_ulp < 1.0e-5); // to-ULP (division + ln/exp), like DAIS/ocean

    // Quality: MBOIT vs exact, and WBOIT vs exact, on the SAME dense scene (RGBA8 LSB). The hero tier wins at high complexity.
    const auto q = [](double v) {
        double c = v;
        if (c < 0.0) { c = 0.0; }
        else if (c > 1.0) { c = 1.0; }
        return static_cast<int>(std::lround(c * 255.0));
    };
    int        mboit_err = 0;
    for (int ch = 0; ch < 3; ++ch)
    {
        const int d = std::abs(q(mb_cpu[static_cast<crd::usize>(ch)]) - q(exact_cpu[static_cast<crd::usize>(ch)]));
        if (d > mboit_err) { mboit_err = d; }
    }
    const crd::u32 wboit_err = crd::gputest::rgba8_max_channel_diff(crd::gputest::wboit_oracle_pixel(scene),
                                                                    crd::gputest::oit_exact_composite_rgba8(scene));
    INFO("2-layer: MBOIT vs exact = " << mboit_err << " LSB · WBOIT vs exact = " << wboit_err << " LSB");
    CHECK(mboit_err <= 1);                               // 4 power moments resolve 2 depth masses EXACTLY (± the ln/exp ULP)
    CHECK(static_cast<crd::u32>(mboit_err) < wboit_err); // ...capturing depth ordering WBOIT's crude single weight cannot
}

// D-007 B17-b (extension): 6-POWER-MOMENT MBOIT on Vulkan — the hero tier LIFTED to 3-mass depth complexity via the larger
// 4×4 Hankel Cholesky + a cubic root-solve + Gauss-Radau form factor (`oit::msm_hamburger6_scalar`). 6 moments resolve 3
// depth masses EXACTLY ⇒ MBOIT is bit-exact at 3-layer complexity, where WBOIT's single weight cannot order the layers.
TEST_CASE("D-007 B17-b: 6-moment MBOIT on Vulkan (larger Cholesky + cubic -- exact at 3 masses, beats WBOIT)",
          "[gpu-context][vulkan][gpu][oit][compute]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg2;
    cfg2.backend  = gpu::GpuBackend::Vulkan;
    cfg2.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(cfg2);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator   alloc(32U << 20U);
    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 3U; // 6 power moments resolve 3 depth masses EXACTLY
    acfg.local_size = 64U;
    crd::gputest::WboitScene scene{};
    scene.count = 3U;
    scene.background[0] = 0.05F; scene.background[1] = 0.06F; scene.background[2] = 0.08F;
    scene.color[0][0] = 0.92F; scene.color[0][1] = 0.10F; scene.color[0][2] = 0.10F; scene.alpha[0] = 0.60F; scene.depth[0] = 0.20F;
    scene.color[1][0] = 0.10F; scene.color[1][1] = 0.90F; scene.color[1][2] = 0.12F; scene.alpha[1] = 0.50F; scene.depth[1] = 0.50F;
    scene.color[2][0] = 0.10F; scene.color[2][1] = 0.12F; scene.color[2][2] = 0.92F; scene.alpha[2] = 0.70F; scene.depth[2] = 0.80F;
    acfg.bg[0] = scene.background[0];
    acfg.bg[1] = scene.background[1];
    acfg.bg[2] = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_glsl(gr, en, &alloc, kern)) { emit_ok = false; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "ckir_mboit6", &alloc);
        if (!spv.ok) { emit_ok = false; }
        return compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> mb_cpu(&alloc);
    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    mb_gpu(&alloc);
    crd::kir_test::mboit6_oracle(acfg, scene, alloc, mb_cpu);
    crd::kir_test::mboit6_dispatch(compute, make_pipe, acfg, scene, alloc, mb_gpu);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu);
    REQUIRE(emit_ok);
    REQUIRE(mb_gpu.size() == mb_cpu.size());

    double worst_ulp = 0.0;
    for (crd::usize i = 0; i < mb_gpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(mb_gpu[i]) - mb_cpu[i]);
        if (d > worst_ulp) { worst_ulp = d; }
    }
    INFO("MBOIT6 GPU vs oracle worst |Δ| = " << worst_ulp);
    CHECK(worst_ulp < 5.0e-3); // to-ULP: the cubic root-solve amplifies the GPU/CPU transcendental ULP (~1 LSB in 8-bit)

    const auto q = [](double v) {
        double c = v;
        if (c < 0.0) { c = 0.0; }
        else if (c > 1.0) { c = 1.0; }
        return static_cast<int>(std::lround(c * 255.0));
    };
    int        mboit_err = 0;
    for (int ch = 0; ch < 3; ++ch)
    {
        const int d = std::abs(q(mb_cpu[static_cast<crd::usize>(ch)]) - q(exact_cpu[static_cast<crd::usize>(ch)]));
        if (d > mboit_err) { mboit_err = d; }
    }
    const crd::u32 wboit_err = crd::gputest::rgba8_max_channel_diff(crd::gputest::wboit_oracle_pixel(scene),
                                                                    crd::gputest::oit_exact_composite_rgba8(scene));
    INFO("3-layer: MBOIT6 vs exact = " << mboit_err << " LSB · WBOIT vs exact = " << wboit_err << " LSB");
    CHECK(mboit_err <= 3);                                     // 6 power moments resolve 3 depth masses ~EXACTLY (1 LSB)
    CHECK(static_cast<crd::u32>(mboit_err) + 8U < wboit_err);  // ...DECISIVELY beating WBOIT at 3-layer depth complexity
}

// D-007 B17-c (scalable): the ATOMIC LINKED-LIST A-buffer on Vulkan — the DEPLOYABLE fragment capture (Carpenter 1984, GPU
// form) enabled by NEW value-returning atomics in CKIR (`atomicAdd(counter,1)` node allocator + `atomicExchange(head,slot)`
// list push). Fragments race into per-pixel linked lists (order nondeterministic); the resolve WALKS + SORTS ⇒ the composite
// is deterministic and must match the static-slot EXACT reference BIT-FOR-BIT — proving the dynamic capture loses no fragments.
TEST_CASE("D-007 B17-c: scalable atomic linked-list A-buffer on Vulkan (value-returning atomics == exact reference)",
          "[gpu-context][vulkan][gpu][oit][compute][atomic]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg2;
    cfg2.backend  = gpu::GpuBackend::Vulkan;
    cfg2.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(cfg2);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator   alloc(16U << 20U);
    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 4U;
    acfg.local_size = 64U;
    const auto scene = crd::gputest::make_oit_scene();
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_glsl(gr, en, &alloc, kern)) { emit_ok = false; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "ckir_abuffer_atomic", &alloc);
        if (!spv.ok) { emit_ok = false; }
        return compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
    };

    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    gpu_out(&alloc);
    crd::kir_test::abuffer_atomic_dispatch(compute, make_pipe, acfg, scene, alloc, gpu_out);
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu); // the static-slot exact reference
    REQUIRE(emit_ok);
    REQUIRE(gpu_out.size() == exact_cpu.size());

    double worst = 0.0;
    for (crd::usize i = 0; i < gpu_out.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(gpu_out[i]) - exact_cpu[i]);
        if (d > worst) { worst = d; }
    }
    INFO("atomic A-buffer vs static-slot exact reference: worst |Δ| = " << worst);
    CHECK(worst == 0.0); // dynamic atomic capture + sort == the exact composite, bit-for-bit
    CHECK(std::fabs(exact_cpu[0] - static_cast<double>(scene.background[0])) > 0.05); // transparency actually composited
}

// D-007 B17-c (scalable): STOCHASTIC TRANSPARENCY on Vulkan (Enderton 2010) — the cheap UNBOUNDED-depth tier (no list, no
// sort, no moment budget). S sub-samples per pixel, each keeping the nearest fragment a DETERMINISTIC hash stochastically
// covers; the mean is an UNBIASED estimate of the exact `over`. Two claims proven: (1) the deterministic integer hash makes
// the "random" result BIT-EXACT vs the CPU oracle (portable/reproducible ⇒ TAA history is consistent across backends); and
// (2) E[stochastic] == the exact A-buffer, so averaging converges to the exact composite (the pixel-averaged estimate is
// sub-LSB, and per-pixel noise shrinks with S — the noise TAA resolves over frames).
TEST_CASE("D-007 B17-c: stochastic transparency on Vulkan (deterministic-hash coverage; unbiased == exact A-buffer)",
          "[gpu-context][vulkan][gpu][oit][compute][stochastic]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg2;
    cfg2.backend  = gpu::GpuBackend::Vulkan;
    cfg2.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(cfg2);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator   alloc(64U << 20U);
    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 32U;
    acfg.height     = 32U;
    acfg.layers     = 4U;
    acfg.local_size = 64U;
    const auto scene = crd::gputest::make_oit_scene();
    acfg.bg[0]       = scene.background[0];
    acfg.bg[1]       = scene.background[1];
    acfg.bg[2]       = scene.background[2];

    bool       emit_ok   = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_glsl(gr, en, &alloc, kern)) { emit_ok = false; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "ckir_stochastic", &alloc);
        if (!spv.ok) { emit_ok = false; }
        return compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
    };

    // (1) BIT-EXACT: the GPU stochastic result == the CPU oracle to the last bit (deterministic hash + per-op f32 rounding).
    acfg.samples = 32U;
    crd::containers::Array<crd::f64> st_cpu(&alloc);
    crd::containers::Array<crd::f64> exact_cpu(&alloc);
    crd::containers::Array<float>    st_gpu(&alloc);
    crd::kir_test::stochastic_oracle(acfg, scene, alloc, st_cpu);
    crd::kir_test::stochastic_dispatch(compute, make_pipe, acfg, scene, alloc, st_gpu);
    REQUIRE(emit_ok);
    REQUIRE(st_gpu.size() == st_cpu.size());
    double worst = 0.0;
    for (crd::usize i = 0; i < st_gpu.size(); ++i)
    {
        const double d = std::fabs(static_cast<double>(st_gpu[i]) - st_cpu[i]);
        if (d > worst) { worst = d; }
    }
    INFO("stochastic GPU vs oracle worst |Δ| = " << worst);
    CHECK(worst == 0.0); // a DETERMINISTIC "random" tier ⇒ bit-identical across backends (portable TAA history)

    // (2) UNBIASED + CONVERGENT: E[stochastic] == the exact A-buffer. Metric per S: `bias` = |mean-over-pixels − exact| (the
    // W*H*S-sample estimate of the composite → ~0 as unbiasedness demands) and `rms` = per-pixel RMS error (the Monte-Carlo
    // NOISE, ~σ/√S — the noise TAA resolves over frames). Measured on the (bit-identical) oracle, so it equally certifies the
    // GPU result. S=256 reuses the already-computed `st_cpu`; only S=16 needs a fresh (cheaper) oracle run.
    crd::kir_test::abuffer_oracle(acfg, scene, alloc, exact_cpu); // the exact `over` reference
    const crd::u32 wh = acfg.width * acfg.height;
    const auto     measure = [&](const crd::containers::Array<crd::f64>& s_cpu, double& bias, double& rms) {
        double mean[3] = {0.0, 0.0, 0.0};
        double sse     = 0.0;
        for (crd::u32 p = 0; p < wh; ++p)
        {
            for (int ch = 0; ch < 3; ++ch)
            {
                const double v = s_cpu[static_cast<crd::usize>(p) * 3U + static_cast<crd::usize>(ch)];
                const double d = v - exact_cpu[static_cast<crd::usize>(ch)];
                mean[ch] += v;
                sse += d * d;
            }
        }
        bias = 0.0;
        for (int ch = 0; ch < 3; ++ch)
        {
            const double b = std::fabs(mean[ch] / static_cast<double>(wh) - exact_cpu[static_cast<crd::usize>(ch)]);
            if (b > bias) { bias = b; }
        }
        rms = std::sqrt(sse / static_cast<double>(wh * 3U));
    };
    double bias_hi = 0.0;
    double rms_hi  = 0.0;
    double bias_lo = 0.0;
    double rms_lo  = 0.0;
    measure(st_cpu, bias_hi, rms_hi); // S=32 (reuse the bit-exact run's oracle)
    crd::kir::oit::AbufferConfig c_lo = acfg;
    c_lo.samples                      = 8U; // 4× fewer samples ⇒ theory predicts √4 = 2× MORE noise
    crd::containers::Array<crd::f64> s_lo(&alloc);
    crd::kir_test::stochastic_oracle(c_lo, scene, alloc, s_lo);
    measure(s_lo, bias_lo, rms_lo);
    INFO("stochastic: bias(S=8)=" << bias_lo << " rms(S=8)=" << rms_lo << " | bias(S=32)=" << bias_hi
                                  << " rms(S=32)=" << rms_hi);
    CHECK(bias_hi < 0.02);          // UNBIASED: the W*H*32-sample pixel-average lands within ~0.02 of the exact composite
    CHECK(rms_hi < rms_lo);         // CONVERGENCE: per-pixel Monte-Carlo noise shrinks with more samples (TAA frames)
    CHECK(rms_hi < 0.65 * rms_lo);  // ...by ~√(32/8) = 2× the theory predicts (threshold 0.65 leaves room for MC variance)
    CHECK(std::fabs(exact_cpu[0] - static_cast<double>(scene.background[0])) > 0.05); // transparency actually composited
}

// D-007 B17-c: OIT tier GPU PERF BOARD — kernel-only cost per tier on a HIGH-OVERDRAW scene (512x512 px, 8 translucent
// layers), GPU-timed via `last_gpu_ms` (brackets only the recorded compute dispatches; upload/readback excluded, like the
// FFT/GI benches), min-of-30. Complements the accuracy board. Hidden ([.oit-bench]) — run explicitly. Prints a table to
// transcribe into docs/bench. The shared deferred STORE is timed once (the fragment-capture cost the moment/A-buffer resolves
// share); each resolve is then timed reading the prefilled store. The atomic tier clears its head buffer (untimed) each run.
TEST_CASE("D-007 B17-c: OIT tier GPU PERFORMANCE (Vulkan, last_gpu_ms, min-of-30, 8-layer high-overdraw)", "[.oit-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(256U << 20U);

    crd::kir::oit::AbufferConfig acfg;
    acfg.width      = 1024U; // high overdraw via resolution (1M pixels); layers=4 keeps the UNROLLED sort-network emit
    acfg.height     = 1024U; // tractable (8-layer's 28 compare-exchanges explode the inline-select expansion — a separate
    acfg.layers     = 4U;    // emitter axis; the tier COMPARISON is valid at 4-layer depth × 1M pixels = 4M fragments)
    acfg.local_size = 64U;
    acfg.samples    = 32U;
    crd::gputest::WboitScene scene{};
    scene.count         = 4U;
    scene.background[0] = 0.10F;
    scene.background[1] = 0.10F;
    scene.background[2] = 0.12F;
    for (crd::u32 q = 0; q < 8U; ++q)
    {
        const float f     = static_cast<float>(q);
        scene.color[q][0] = 0.12F + 0.10F * f;
        scene.color[q][1] = 0.90F - 0.08F * f;
        scene.color[q][2] = 0.20F + 0.09F * f;
        scene.alpha[q]    = 0.30F + 0.03F * f;
        scene.depth[q]    = 0.08F + 0.11F * f; // distinct ascending depths (a strict order for the sort/z-test)
    }
    acfg.bg[0] = scene.background[0];
    acfg.bg[1] = scene.background[1];
    acfg.bg[2] = scene.background[2];

    const crd::u32 wh    = acfg.width * acfg.height;
    const crd::u32 total = wh * acfg.layers;

    const auto pipe_of = [&](kir::KGraph& g, const kir::KEntry& e, const char* nm, int nb) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), nm, &alloc);
        REQUIRE(spv.ok);
        auto p = compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
        REQUIRE(p != nullptr);
        return p;
    };
    kir::KGraph g_store(&alloc);
    kir::KGraph g_ares(&alloc);
    kir::KGraph g_m4(&alloc);
    kir::KGraph g_m6(&alloc);
    kir::KGraph g_ab(&alloc);
    kir::KGraph g_ar(&alloc);
    kir::KGraph g_st(&alloc);
    const auto  e_store = kir::oit::build_abuffer_store(g_store, acfg);
    const auto  e_ares  = kir::oit::build_abuffer_resolve(g_ares, acfg);
    const auto  e_m4    = kir::oit::build_mboit_resolve(g_m4, acfg);
    const auto  e_m6    = kir::oit::build_mboit6_resolve(g_m6, acfg);
    const auto  e_ab    = kir::oit::build_abuffer_atomic_build(g_ab, acfg);
    const auto  e_ar    = kir::oit::build_abuffer_atomic_resolve(g_ar, acfg);
    const auto  e_st    = kir::oit::build_stochastic_resolve(g_st, acfg);
    auto        p_store = pipe_of(g_store, e_store, "bench_store", 6);
    auto        p_ares  = pipe_of(g_ares, e_ares, "bench_ares", 6);
    auto        p_m4    = pipe_of(g_m4, e_m4, "bench_m4", 6);
    auto        p_m6    = pipe_of(g_m6, e_m6, "bench_m6", 6);
    auto        p_ab    = pipe_of(g_ab, e_ab, "bench_abuild", 5);
    auto        p_ar    = pipe_of(g_ar, e_ar, "bench_aresolve", 4);
    auto        p_st    = pipe_of(g_st, e_st, "bench_stoch", 2);

    const auto mkf = [&](crd::u64 floats) {
        return compute.create_buffer(floats * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    };
    auto sc       = mkf(static_cast<crd::u64>(acfg.layers) * 5U);
    auto nr       = mkf(total);
    auto ng       = mkf(total);
    auto nb       = mkf(total);
    auto na       = mkf(total);
    auto nd       = mkf(total);
    auto out      = mkf(static_cast<crd::u64>(wh) * 3U);
    auto counter  = mkf(1);
    auto head     = mkf(wh);
    auto nnext    = mkf(total);
    auto nodedata = mkf(static_cast<crd::u64>(total) * 5U);

    { // upload the scene once
        auto  up = compute.create_buffer(static_cast<crd::u64>(acfg.layers) * 5U * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* sp = static_cast<float*>(up->map());
        for (crd::u32 q = 0; q < acfg.layers; ++q)
        {
            sp[q * 5U + 0U] = scene.color[q][0]; sp[q * 5U + 1U] = scene.color[q][1]; sp[q * 5U + 2U] = scene.color[q][2];
            sp[q * 5U + 3U] = scene.alpha[q];    sp[q * 5U + 4U] = scene.depth[q];
        }
        up->unmap();
        auto& rec = compute.begin();
        rec.copy(*up, *sc, 0U, 0U, static_cast<crd::u64>(acfg.layers) * 5U * sizeof(float));
        compute.submit_and_wait();
    }
    // reusable resets for the atomic tier: `clr_head` = wh words all EMPTY (head buffer), `clr_cnt` = 1 word 0 (counter)
    auto  clr_head = compute.create_buffer(static_cast<crd::u64>(wh) * sizeof(crd::u32), transfer_src, cg::ComputeMemory::CpuToGpu);
    auto* hp       = static_cast<crd::u32*>(clr_head->map());
    for (crd::u32 i = 0; i < wh; ++i) { hp[i] = crd::kir::oit::kAbufferEmpty; }
    clr_head->unmap();
    auto  clr_cnt = compute.create_buffer(sizeof(crd::u32), transfer_src, cg::ComputeMemory::CpuToGpu);
    *static_cast<crd::u32*>(clr_cnt->map()) = 0U;
    clr_cnt->unmap();

    const crd::u32 g_store_wg = (total + acfg.local_size - 1U) / acfg.local_size;
    const crd::u32 g_pix_wg   = (wh + acfg.local_size - 1U) / acfg.local_size;

    { // prefill the shared deferred store ONCE (untimed) so the resolve-only tiers read a valid fragment store
        cg::ComputeBuffer* b[6] = {sc.get(), nr.get(), ng.get(), nb.get(), na.get(), nd.get()};
        auto&              rec  = compute.begin();
        rec.dispatch(*p_store, crd::containers::ConstSpan<cg::ComputeBuffer*>(b, 6), nullptr, 0U, g_store_wg, 1U, 1U);
        compute.submit_and_wait();
    }

    const auto time_one = [&](cg::ComputePipeline& pipe, cg::ComputeBuffer** binds, int nb2, crd::u32 gx) {
        double best = 1.0e30;
        for (int r = 0; r < 30; ++r)
        {
            auto& rec = compute.begin();
            rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(nb2)), nullptr, 0U, gx, 1U, 1U);
            compute.submit_and_wait();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < best) { best = ms; }
        }
        return best;
    };

    cg::ComputeBuffer* res_bind[6] = {nr.get(), ng.get(), nb.get(), na.get(), nd.get(), out.get()};
    cg::ComputeBuffer* store_bind[6] = {sc.get(), nr.get(), ng.get(), nb.get(), na.get(), nd.get()};
    cg::ComputeBuffer* stoch_bind[2] = {sc.get(), out.get()};

    const double t_store = time_one(*p_store, store_bind, 6, g_store_wg);
    const double t_ares  = time_one(*p_ares, res_bind, 6, g_pix_wg);
    const double t_m4    = time_one(*p_m4, res_bind, 6, g_pix_wg);
    const double t_m6    = time_one(*p_m6, res_bind, 6, g_pix_wg);
    const double t_st    = time_one(*p_st, stoch_bind, 2, g_pix_wg);

    // atomic tier: reset counter+head each run (untimed), then time build + resolve together
    double t_atomic = 1.0e30;
    for (int r = 0; r < 30; ++r)
    {
        {
            auto& rec = compute.begin();
            rec.copy(*clr_cnt, *counter, 0U, 0U, sizeof(crd::u32)); // counter ← 0
            rec.copy(*clr_head, *head, 0U, 0U, static_cast<crd::u64>(wh) * sizeof(crd::u32)); // head ← EMPTY
            compute.submit_and_wait();
        }
        auto&              rec = compute.begin();
        cg::ComputeBuffer* bb[5] = {sc.get(), counter.get(), head.get(), nnext.get(), nodedata.get()};
        rec.dispatch(*p_ab, crd::containers::ConstSpan<cg::ComputeBuffer*>(bb, 5), nullptr, 0U, g_store_wg, 1U, 1U);
        rec.barrier(*head, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        rec.barrier(*nnext, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        rec.barrier(*nodedata, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
        cg::ComputeBuffer* rb[4] = {head.get(), nnext.get(), nodedata.get(), out.get()};
        rec.dispatch(*p_ar, crd::containers::ConstSpan<cg::ComputeBuffer*>(rb, 4), nullptr, 0U, g_pix_wg, 1U, 1U);
        compute.submit_and_wait();
        const double ms = compute.last_gpu_ms();
        if (ms > 0.0 && ms < t_atomic) { t_atomic = ms; }
    }

    std::printf("\n=== OIT tier GPU perf board (Vulkan, kernel-only last_gpu_ms, min-of-30) ===\n");
    std::printf("scene: %ux%u px, %u translucent layers (%u fragments total), samples=%u\n", acfg.width, acfg.height,
                acfg.layers, total, acfg.samples);
    std::printf("  %-26s %8.4f ms   (shared deferred fragment capture)\n", "STORE (shared)", t_store);
    std::printf("  %-26s %8.4f ms   store+resolve = %.4f ms\n", "A-buffer resolve (sort)", t_ares, t_store + t_ares);
    std::printf("  %-26s %8.4f ms   store+resolve = %.4f ms\n", "MBOIT-4 resolve", t_m4, t_store + t_m4);
    std::printf("  %-26s %8.4f ms   store+resolve = %.4f ms\n", "MBOIT-6 resolve", t_m6, t_store + t_m6);
    std::printf("  %-26s %8.4f ms   (build+resolve, head-clear untimed)\n", "A-buffer atomic (list)", t_atomic);
    std::printf("  %-26s %8.4f ms   (S=%u; per-frame TAA cost @S=1 ~ %.4f ms)\n", "Stochastic", t_st, acfg.samples,
                t_st / static_cast<double>(acfg.samples));
    std::printf("============================================================================\n");

    CHECK(t_store < 1.0e29);
    CHECK(t_ares < 1.0e29);
    CHECK(t_atomic < 1.0e29);
    CHECK(t_st < 1.0e29);
}

// D-007 B4: the SAME triangle emitted by a MESH shader (the modern amplification path) — CKIR mesh entry → GL_EXT_mesh_shader
// → a mesh shader object → vkCmdDrawMeshTasksEXT → pixels. Proves the whole mesh pipeline end-to-end. The center must be red
// (inside the mesh-emitted triangle) and a corner blue (the clear) — identical to the vertex-pull result above.
TEST_CASE("D-007 B4: IR-authored MESH-shader triangle draws on Vulkan (CKIR mesh entry -> mesh shader object -> pixels)",
          "[gpu-context][vulkan][gpu][raster][mesh]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("adapter has no VK_EXT_shader_object; skipping"); return; }
    if (!vk->mesh_shader()) { WARN("adapter has no VK_EXT_mesh_shader; skipping the mesh draw"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_triangle_mesh(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    auto mesh = ctx->create_program(mg, me); // KIR mesh entry -> GL_EXT_mesh_shader GLSL -> SPIR-V, behind the seam
    auto fs   = ctx->create_program(fg, fe);
    REQUIRE(mesh != nullptr);
    REQUIRE(fs != nullptr);
    REQUIRE(mesh->stage() == gpu::ShaderStage::Mesh);

    auto program = raster->create_mesh_program(*mesh, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);

    raster->draw_mesh(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 1U); // one meshlet workgroup

    const crd::u32 centre = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 corner = target->read_pixel(0U, 0U);
    CHECK((centre & 0xFFU) >= 250U);          // R high  ⇒ red (inside the MESH-emitted triangle)
    CHECK(((centre >> 16U) & 0xFFU) <= 5U);   // B low
    CHECK((corner & 0xFFU) <= 5U);            // R low   ⇒ blue clear (outside)
    CHECK(((corner >> 16U) & 0xFFU) >= 250U); // B high
}

// D-007 B4: the TASK / AMPLIFICATION path on Vulkan. ONE task workgroup runs EmitMeshTasksEXT(N) to launch N mesh workgroups
// (GPU-driven amplification) and passes a single-uint PAYLOAD; each mesh workgroup renders one triangle coloured by the payload.
// Proves both amplification (draw with a TASK-group count of 1 ⇒ N triangles) and the task→mesh payload channel (red == payload).
TEST_CASE("D-007 B4: Vulkan TASK amplification -- 1 task workgroup emits N mesh triangles + payload",
          "[gpu-context][vulkan][gpu][raster][mesh][task]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object() || !vk->mesh_shader()) { WARN("no shader_object/mesh_shader; skipping the task draw"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    constexpr crd::u32 n_tri   = 4U;
    constexpr crd::u32 payload = 220U;
    kir::KGraph        tg(&alloc);
    kir::KEntry        te;
    crd::gputest::build_task_amplify(tg, te, n_tri, payload);
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_mesh_amplified_tri(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_amplify_fs(fg, fe);

    auto task = ctx->create_program(tg, te); // CKIR task entry -> GL_EXT_mesh_shader task GLSL -> SPIR-V
    auto mesh = ctx->create_program(mg, me);
    auto fs   = ctx->create_program(fg, fe);
    REQUIRE(task != nullptr);
    REQUIRE(mesh != nullptr);
    REQUIRE(fs != nullptr);
    REQUIRE(task->stage() == gpu::ShaderStage::Task);

    auto program = raster->create_task_mesh_program(*task, *mesh, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.2F, 1.0F}, 1U); // 1 TASK workgroup ⇒ N mesh triangles

    // The N task-amplified triangles sit at clip x = -0.7 + c·0.45; each is coloured payload/255 (red). All N must be present
    // (amplification), and the red intensity must be ≈ payload (the task→mesh payload flowed).
    int lit = 0;
    for (crd::u32 c = 0; c < n_tri; ++c)
    {
        const double   xc = -0.7 + static_cast<double>(c) * 0.45;
        const crd::u32 sx = static_cast<crd::u32>((xc + 1.0) * 0.5 * static_cast<double>(dim));
        if ((target->read_pixel(sx, dim / 2U) & 0xFFU) > 180U) { ++lit; } // red ≈ payload(220)
    }
    CHECK(lit == static_cast<int>(n_tri)); // all N amplified triangles rendered with the payload colour
}

// D-007 B4: the MULTI-FIELD task→mesh payload — a task passes a 3-uint payload (v0,v1,v2), each read by the mesh via
// KBuiltin::TaskPayload{,1,2} and coloured into R/G/B. Proves the payload channel carries richer per-meshlet data (bounds /
// LOD / material), not just one uint — the centre pixel's R,G,B must equal the three payload fields.
TEST_CASE("D-007 B4: Vulkan TASK multi-field payload -- a 3-uint payload flows task->mesh",
          "[gpu-context][vulkan][gpu][raster][mesh][task]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object() || !vk->mesh_shader()) { WARN("no shader_object/mesh_shader; skipping"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    constexpr crd::u32 pay_r = 200U;
    constexpr crd::u32 pay_g = 120U;
    constexpr crd::u32 pay_b = 60U;
    kir::KGraph        tg(&alloc);
    kir::KEntry        te;
    crd::gputest::build_task_amplify_rgb(tg, te, 1U, pay_r, pay_g, pay_b);
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_mesh_amplified_rgb(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_amplify_rgb_fs(fg, fe);

    auto task = ctx->create_program(tg, te);
    auto mesh = ctx->create_program(mg, me);
    auto fs   = ctx->create_program(fg, fe);
    REQUIRE(task != nullptr);
    REQUIRE(mesh != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_task_mesh_program(*task, *mesh, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.1F, 1.0F}, 1U);

    const crd::u32 px = target->read_pixel(dim / 2U, dim / 2U); // inside the centred triangle
    CHECK((px & 0xFFU) > pay_r - 6U);           // R ≈ payload v0
    CHECK((px & 0xFFU) < pay_r + 6U);
    CHECK(((px >> 8U) & 0xFFU) > pay_g - 6U);   // G ≈ payload v1
    CHECK(((px >> 8U) & 0xFFU) < pay_g + 6U);
    CHECK(((px >> 16U) & 0xFFU) > pay_b - 6U);  // B ≈ payload v2 — all three fields flowed
    CHECK(((px >> 16U) & 0xFFU) < pay_b + 6U);
}

// D-007 B4: GPU-DRIVEN INDIRECT MESHLET DISPATCH. A compute CULL pass tests 8 meshlets (5 visible) and writes the survivor
// count straight into an INDIRECT-dispatch args buffer; vkCmdDrawMeshTasksIndirectEXT then launches EXACTLY that many mesh
// workgroups — the count decided entirely on the GPU (the CPU never sets it). Proves the Nanite scale loop: only the 5
// surviving meshlets render their triangle; the 3 culled ones never dispatch.
TEST_CASE("D-007 B4: Vulkan GPU-driven indirect meshlet dispatch -- a compute cull writes the dispatch count",
          "[gpu-context][vulkan][gpu][raster][mesh][indirect]")
{
    namespace kir = crd::kir;
    namespace vb  = crd::kir::visbuffer;
    namespace cu  = crd::gpu::compute_usage;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object() || !vk->mesh_shader()) { WARN("no shader_object/mesh_shader; skipping"); return; }
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // 1) the compute CULL: 8 meshlets, keys[i] = (i < 5) ⇒ visible → 5 survivors written into args[0] (groupCountX).
    constexpr crd::u32       n_meshlets = 8U;
    constexpr crd::u32       survivors  = 5U;
    vb::MeshletCullConfig     ccfg;
    ccfg.n_meshlets       = n_meshlets;
    ccfg.local_size       = 64U;
    kir::KGraph            cg(&alloc);
    const kir::KEntry      ce = vb::build_meshlet_cull(cg, ccfg);
    kir::GlslKernel        kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(cg, ce, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "cull", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    auto keys_dev = compute.create_buffer(n_meshlets * 4U, cu::storage | cu::transfer_dst, gpu::ComputeMemory::GpuOnly);
    auto args_dev = compute.create_buffer(3U * 4U, cu::storage | cu::indirect | cu::transfer_dst | cu::transfer_src,
                                          gpu::ComputeMemory::GpuOnly); // INDIRECT ⇒ CONCURRENT-shared for the mesh draw
    auto keys_up  = compute.create_buffer(n_meshlets * 4U, cu::transfer_src, gpu::ComputeMemory::CpuToGpu);
    auto args_up  = compute.create_buffer(3U * 4U, cu::transfer_src, gpu::ComputeMemory::CpuToGpu);
    auto args_rb  = compute.create_buffer(3U * 4U, cu::transfer_dst, gpu::ComputeMemory::GpuToCpu);
    REQUIRE(args_dev != nullptr);
    auto* kp = static_cast<crd::u32*>(keys_up->map());
    for (crd::u32 i = 0; i < n_meshlets; ++i) { kp[i] = (i < survivors) ? 1U : 0U; }
    keys_up->unmap();
    auto* ap = static_cast<crd::u32*>(args_up->map());
    ap[0] = 0U;
    ap[1] = 1U;
    ap[2] = 1U; // {groupCountX = 0 (accumulated), groupCountY = 1, groupCountZ = 1}
    args_up->unmap();

    auto&                  rec      = compute.begin();
    crd::gpu::ComputeBuffer* binds[2] = {keys_dev.get(), args_dev.get()};
    rec.copy(*keys_up, *keys_dev, 0U, 0U, n_meshlets * 4U);
    rec.copy(*args_up, *args_dev, 0U, 0U, 3U * 4U);
    rec.barrier(*keys_dev, crd::gpu::ComputeAccess::TransferDst, crd::gpu::ComputeAccess::ShaderRead);
    rec.barrier(*args_dev, crd::gpu::ComputeAccess::TransferDst, crd::gpu::ComputeAccess::ShaderRead);
    rec.dispatch(*pipe, crd::containers::ConstSpan<crd::gpu::ComputeBuffer*>(binds, 2), nullptr, 0U, 1U, 1U, 1U);
    rec.barrier(*args_dev, crd::gpu::ComputeAccess::ShaderWrite, crd::gpu::ComputeAccess::TransferSrc);
    rec.copy(*args_dev, *args_rb, 0U, 0U, 3U * 4U);
    compute.submit_and_wait();

    const auto* arp   = static_cast<const crd::u32*>(args_rb->map());
    const crd::u32 count = arp[0];
    args_rb->unmap();
    CHECK(count == survivors); // the compute cull computed the mesh-dispatch count on the GPU

    // 2) the raster INDIRECT mesh draw reads args_dev (the compute-written count) — GPU-driven, no CPU-set group count.
    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_mesh_grid_tri(mg, me);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_amplify_fs(fg, fe);
    auto meshp = ctx->create_program(mg, me);
    auto fsp   = ctx->create_program(fg, fe);
    REQUIRE(meshp != nullptr);
    REQUIRE(fsp != nullptr);
    auto program = raster->create_mesh_program(*meshp, *fsp);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_mesh_indirect(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.1F, 1.0F}, args_dev->native_handle(), 0U);

    int rendered = 0;
    int culled   = 0;
    for (crd::u32 w = 0; w < n_meshlets; ++w) // meshlet w renders at clip x = -0.8 + w*0.2
    {
        const double   xc  = -0.8 + static_cast<double>(w) * 0.2;
        const crd::u32 sx  = static_cast<crd::u32>((xc + 1.0) * 0.5 * static_cast<double>(dim));
        const bool     red = (target->read_pixel(sx, dim / 2U) & 0xFFU) > 180U;
        if (w < survivors) { if (red) { ++rendered; } }
        else if (!red) { ++culled; }
    }
    CHECK(rendered == static_cast<int>(survivors));               // all 5 survivors rendered via the indirect count
    CHECK(culled == static_cast<int>(n_meshlets - survivors));    // the 3 culled meshlets never dispatched
}

// D-007 B4-tess: the PORTABLE displacement path — VS→TESS-CONTROL→TESS-EVAL→FS. The VS emits a 4-corner quad patch (±0.6),
// the hull sets 8x8 tess levels, the domain reads TessPatchPosition (the bilerp) + EXPANDS the quad x1.3 (a per-vertex domain
// transform), the FS paints it red. Proves the tessellator ran end to end: a pixel between the base edge (0.6) and the
// expanded edge (0.78) is red ONLY because the domain shader displaced the generated vertices.
TEST_CASE("D-007 B4-tess: Vulkan tessellation -- a VS->TCS->TES->FS quad subdivides + displaces",
          "[gpu-context][vulkan][gpu][raster][tess]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object() || !vk->tessellation()) { WARN("no shader_object/tessellation; skipping the tess draw"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_tess_quad_vs(vg, ve);
    kir::KGraph cg(&alloc);
    kir::KEntry ce;
    crd::gputest::build_tess_hull(cg, ce);
    kir::KGraph eg(&alloc);
    kir::KEntry ee;
    crd::gputest::build_tess_domain(eg, ee);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);

    auto vs  = ctx->create_program(vg, ve);
    auto tcs = ctx->create_program(cg, ce); // CKIR TessControl entry -> hull GLSL -> SPIR-V
    auto tes = ctx->create_program(eg, ee); // CKIR TessEval entry    -> domain GLSL -> SPIR-V
    auto fs  = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(tcs != nullptr);
    REQUIRE(tes != nullptr);
    REQUIRE(fs != nullptr);
    REQUIRE(tcs->stage() == gpu::ShaderStage::TessControl);
    REQUIRE(tes->stage() == gpu::ShaderStage::TessEval);

    auto program = raster->create_tess_program(*vs, *tcs, *tes, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_tess(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.2F, 1.0F}, 1U); // ONE quad patch

    // Screen x for a clip x: sx = (x+1)/2·dim. The base quad edge is 0.6 (sx=51); the domain-expanded edge is 0.78 (sx=57).
    CHECK((target->read_pixel(dim / 2U, dim / 2U) & 0xFFU) >= 250U); // centre red — the quad renders
    const crd::u32 sx_expanded = static_cast<crd::u32>((0.72 + 1.0) * 0.5 * static_cast<double>(dim)); // clip x≈0.72 (sx≈55)
    CHECK((target->read_pixel(sx_expanded, dim / 2U) & 0xFFU) >= 250U); // between base (0.6) + expanded (0.78) — domain ran
    CHECK((target->read_pixel(63U, dim / 2U) & 0xFFU) < 40U); // clip x≈0.97, beyond the expanded quad — still the blue clear
}

// D-007 B4-vis-4: the HW-RASTER VISIBILITY BUFFER — the hybrid Nanite path (HW raster wins on big triangles). A fullscreen
// quad (2 triangles) rasterizes into a R32_UINT target whose fragment shader writes gl_PrimitiveID, so every pixel records
// which triangle covered it. Proves KBuiltin::PrimitiveId lowers + the uint visibility target renders + read_pixel returns id.
TEST_CASE("D-007 B4-vis-4: Vulkan HW-raster visibility buffer writes SV_PrimitiveId per pixel",
          "[gpu-context][vulkan][gpu][raster][visbuffer]")
{
    namespace kir = crd::kir;

    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no shader_object; skipping the visbuffer draw"); return; }

    crd::memory::TlsfAllocator alloc(4U << 20U);
    auto                       raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_visbuffer_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_visbuffer_fs(fg, fe);
    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_visbuffer_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw_visbuffer(*target, *program, 0xFFFFFFFFU, 6U); // clear id = empty; 6 verts = 2 triangles

    int n0     = 0;
    int n1     = 0;
    int nempty = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 id = target->read_pixel(x, y);
            if (id == 0U) { ++n0; }
            else if (id == 1U) { ++n1; }
            else if (id == 0xFFFFFFFFU) { ++nempty; }
        }
    }
    CHECK(nempty == 0);                            // the fullscreen quad covers every pixel (no cleared 0xFFFFFFFF survives)
    CHECK(n0 + n1 == static_cast<int>(dim * dim)); // every pixel is primitive id 0 or 1
    CHECK(n0 > static_cast<int>(dim * dim) / 4);   // triangle 0 covers a substantial half (its distinct SV_PrimitiveID)
    CHECK(n1 > static_cast<int>(dim * dim) / 4);   // triangle 1 covers the other half
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

// D-007 B4: PER-PRIMITIVE VRS from a MESH shader — the mesh's gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT output (2×2)
// drives the coarse fragment rate via draw_mesh_vrs (REPLACE combiner). Same ramp FS + coarsening check as the VS case: a 2×2
// rate makes each 2×2 block share one fragment invocation, so horizontal even-x pairs become EQUAL across the gradient.
TEST_CASE("D-007 B4: per-primitive VRS from a MESH shader coarsens shading (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][mesh][vrs]")
{
    namespace kir = crd::kir;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    if (!r.vk->fragment_shading_rate() || !r.vk->mesh_shader()) { WARN("no VRS / mesh_shader; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(4U << 20U);

    kir::KGraph mg(&alloc);
    kir::KEntry me;
    crd::gputest::build_vrs_primitive_mesh(mg, me); // mesh emits gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT = 2x2
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_vrs_ramp_fs(fg, fe);
    auto mesh = r.ctx->create_program(mg, me);
    auto fs   = r.ctx->create_program(fg, fe);
    REQUIRE(mesh != nullptr); // the mesh emits the per-primitive rate ⇒ must compile to valid SPIR-V
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_mesh_program(*mesh, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());

    constexpr crd::u32 dim    = 32U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_mesh_vrs(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 1U);

    const int nmesh = count_equal_even_pairs(*target, dim);
    WARN("[vrs mesh vulkan] n_equal=" << nmesh);
    CHECK(nmesh > static_cast<int>(dim * dim / 4U)); // the mesh's per-primitive 2x2 rate coarsened the ramp
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

// B4-vis: the NANITE software rasterizer (atomicMin visibility buffer) DISPATCHES on Vulkan == CPU oracle bit-exact — the
// SAME kernel + scene + oracle as the DX12 test, so the two backends also agree with each other. One thread per triangle
// writes a per-pixel (depth<<idBits)|triangleId key by atomicMin; the nearer centre triangle wins the overlap.
TEST_CASE("B4-vis: CKIR software rasterizer (atomicMin visibility buffer) DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][visbuffer]")
{
    namespace kir = crd::kir;
    namespace vb  = crd::kir::visbuffer;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator         alloc(8U << 20U);
    const crd::kir_test::SwRasterScene scene = crd::kir_test::make_sw_raster_scene();
    kir::KGraph                        g(&alloc);
    const kir::KEntry                  e = vb::build_sw_raster_visbuffer(g, scene.cfg);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_visraster", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U); // 3 storage bindings, no push
    REQUIRE(pipe != nullptr);

    crd::containers::Array<crd::u32> vis_cpu(&alloc);
    crd::containers::Array<crd::u32> vis_gpu(&alloc);
    crd::kir_test::sw_raster_oracle(g, e, scene, alloc, vis_cpu);
    crd::kir_test::dispatch_visraster(compute, *pipe, scene, vis_gpu);

    const int npix = static_cast<int>(scene.cfg.width * scene.cfg.height);
    int       diff = 0;
    for (int i = 0; i < npix; ++i) { if (vis_gpu[static_cast<crd::usize>(i)] != vis_cpu[static_cast<crd::usize>(i)]) { ++diff; } }
    CHECK(diff == 0); // BIT-EXACT: GPU visibility keys == CPU oracle (atomicMin order-independent; matches the DX12 result)

    const crd::u32 w  = scene.cfg.width;
    const crd::u32 ib = scene.cfg.id_bits;
    const auto     at = [&](crd::u32 x, crd::u32 y) { return vis_gpu[static_cast<crd::usize>(y) * w + x]; };
    CHECK(vb::vis_id(at(16U, 16U), ib) == 2U); // centre: the NEAR triangle (id 2) wins the depth resolve
    CHECK(vb::vis_id(at(10U, 18U), ib) == 1U); // upper-left half of the quad → triangle 1
    CHECK(vb::vis_id(at(20U, 10U), ib) == 0U); // lower-right half of the quad → triangle 0
    CHECK(at(2U, 2U) == vb::kVisEmptyKey);     // outside the geometry → still the empty key
}

// B4-vis-2: DEFERRED ATTRIBUTE INTERPOLATION SHADE (DAIS) DISPATCHES on Vulkan. Reads the visibility buffer, reconstructs
// perspective-correct barycentrics + interpolates a per-vertex attribute once per visible pixel. Visibility is bit-exact
// (B4-vis-1); the deferred shade's single perspective-normalize divide is ~2.5 ULP on f32 hardware, so the reconstruction
// matches the oracle to a TIGHT ULP tolerance. The scene's distinct clip-w proves the /w correction ran (centroid ~8, not 14).
TEST_CASE("B4-vis-2: CKIR deferred attribute shade (DAIS) DISPATCHES on Vulkan == CPU oracle to ULP",
          "[gpu-context][vulkan][gpu][kernel][visbuffer][dais]")
{
    namespace kir = crd::kir;
    namespace vb  = crd::kir::visbuffer;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());

    crd::memory::TlsfAllocator       alloc(16U << 20U);
    const crd::kir_test::DaisScene   scene = crd::kir_test::make_dais_scene();
    crd::containers::Array<crd::u32> vis(&alloc);
    crd::kir_test::dais_make_vis(scene, alloc, vis); // CPU rasterize → visibility keys (the shared input)

    kir::KGraph       dg(&alloc);
    const kir::KEntry de = vb::build_deferred_attr_shade(dg, scene.shade_cfg);
    kir::GlslKernel   kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(dg, de, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_dais", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U); // 5 storage bindings, no push
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> shade_cpu(&alloc);
    crd::containers::Array<float> shade_gpu(&alloc);
    crd::kir_test::dais_oracle(dg, de, scene, vis, alloc, shade_cpu);
    crd::kir_test::dispatch_dais(compute, *pipe, scene, vis, shade_gpu);

    const crd::u32 w    = scene.shade_cfg.width;
    const int      npix = static_cast<int>(w * scene.shade_cfg.height);
    const auto     absf = [](float v) { return v < 0.0F ? -v : v; };
    float          max_rel = 0.0F;
    int            covered = 0;
    int            outrange = 0;
    for (int i = 0; i < npix; ++i)
    {
        const float gp  = shade_gpu[static_cast<crd::usize>(i)];
        const float cp  = shade_cpu[static_cast<crd::usize>(i)];
        const float acp = absf(cp) > 1.0e-6F ? absf(cp) : 1.0e-6F;
        const float rel = absf(gp - cp) / acp;
        if (rel > max_rel) { max_rel = rel; }
        if (gp != 0.0F)
        {
            ++covered;
            if (gp < 1.9F || gp > 32.1F) { ++outrange; }
        }
    }
    WARN("[DAIS vk] max relative error (GPU vs oracle) = " << max_rel);
    CHECK(max_rel < 1.0e-6F); // ≈ a few f32 ULP — the perspective-normalize divide's hardware imprecision
    CHECK(covered > 100);     // the perspective triangle shaded a good fraction of the 32x32
    CHECK(outrange == 0);     // every covered pixel is a valid convex blend of the {2,8,32} attributes
    CHECK(shade_gpu[2U * w + 2U] == 0.0F); // outside → the cleared background
    const float centre = shade_gpu[13U * w + 16U];
    CHECK(centre > 5.0F);
    CHECK(centre < 11.0F); // perspective-correct (~8), decisively below the naive average (14)
}

// B4-vis-3: the HZB two-pass OCCLUSION CULL DISPATCHES on Vulkan == CPU oracle bit-exact. A max-depth mip pyramid (one
// downsample dispatch per level) + a per-cluster AABB-vs-HZB test — MAX + compare are order-independent ⇒ bit-exact. The
// GPU-driven culling that makes a Nanite pipeline scale: a cluster behind the near wall is culled; open / in-front survive.
TEST_CASE("B4-vis-3: CKIR HZB two-pass occlusion cull DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][visbuffer][hzb]")
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

    crd::memory::TlsfAllocator    alloc(16U << 20U);
    const crd::kir_test::HzbScene scene   = crd::kir_test::make_hzb_scene();
    bool                          emit_ok = true;
    const auto make_pipe = [&](const kir::KGraph& gr, const kir::KEntry& en, int nbufs) {
        kir::GlslKernel kern(&alloc);
        if (!kir::emit_compute_kernel_glsl(gr, en, &alloc, kern)) { emit_ok = false; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                    "ckir_hzb", &alloc);
        if (!spv.ok) { emit_ok = false; }
        return compute.create_pipeline_from_spirv(
            crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbufs, 0U);
    };
    crd::containers::Array<crd::u32> vis_cpu(&alloc);
    crd::containers::Array<crd::u32> vis_gpu(&alloc);
    crd::kir_test::hzb_cull_oracle(scene, alloc, vis_cpu);
    crd::kir_test::hzb_cull_dispatch(compute, make_pipe, scene, alloc, vis_gpu);
    REQUIRE(emit_ok); // every HZB / cull kernel lowered to SPIR-V

    for (int c = 0; c < crd::kir_test::HzbScene::n_clusters; ++c)
    {
        CHECK(vis_gpu[static_cast<crd::usize>(c)] == vis_cpu[static_cast<crd::usize>(c)]);       // BIT-EXACT vs oracle
        CHECK(vis_gpu[static_cast<crd::usize>(c)] == scene.expected[static_cast<crd::usize>(c)]); // analytic: cull=0, visible=1
    }
}

TEST_CASE("v17 NRC: CKIR fused-MLP FP32 forward DISPATCHES on Vulkan == CPU oracle BIT-EXACT (the portable moat)",
          "[gpu-context][vulkan][gpu][kernel][mlp]")
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
    kir::MlpConfig             mcfg; // width 64, 6 layers
    mcfg.batch_tile = 64;
    mcfg.warps      = 2;
    kir::KGraph       g(&alloc);
    const kir::KEntry e     = kir::build_mlp_fwd_fp32(g, mcfg);
    const int         wd    = mcfg.width;
    const int         batch = 64;
    const int         n_in  = batch * wd;
    const int         n_w   = mcfg.layers * wd * wd;

    // 1) CPU ORACLE (f64 buffers, F32-rounded ops) — the bit-exact reference.
    crd::containers::Array<crd::f64> in64(&alloc);
    in64.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<crd::f64> w64(&alloc);
    w64.resize(static_cast<crd::usize>(n_w));
    crd::containers::Array<crd::f64> out64(&alloc);
    out64.resize(static_cast<crd::usize>(n_in));
    for (int i = 0; i < n_in; ++i) { in64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(0.2F * static_cast<float>((i * 7) % 13 - 6))); }
    for (int i = 0; i < n_w; ++i) { w64[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(0.1F * static_cast<float>((i * 5) % 11 - 5))); }
    kir::KernelBuffer bufs[3] = {{in64.data(), n_in, 0, 0}, {w64.data(), n_w, 0, 1}, {out64.data(), n_in, 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, e.local_size[0], &alloc, static_cast<crd::u32>(batch));

    // 2) emit kernel GLSL → SPIR-V → pipeline (3 storage bindings, no push).
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_mlp_fp32", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    // 3) dispatch `batch` workgroups on the portable surface, read back.
    crd::containers::Array<float> in32(&alloc);
    in32.resize(static_cast<crd::usize>(n_in));
    crd::containers::Array<float> w32(&alloc);
    w32.resize(static_cast<crd::usize>(n_w));
    crd::containers::Array<float> out32(&alloc);
    out32.resize(static_cast<crd::usize>(n_in));
    for (int i = 0; i < n_in; ++i) { in32[static_cast<crd::usize>(i)] = static_cast<float>(in64[static_cast<crd::usize>(i)]); }
    for (int i = 0; i < n_w; ++i) { w32[static_cast<crd::usize>(i)] = static_cast<float>(w64[static_cast<crd::usize>(i)]); }
    for (int i = 0; i < n_in; ++i) { out32[static_cast<crd::usize>(i)] = -1.0F; }
    float*    host[3] = {in32.data(), w32.data(), out32.data()};
    const int lens[3] = {n_in, n_w, n_in};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 3, static_cast<crd::u32>(batch));

    // 4) GPU == oracle, bit-for-bit (FP32 precise, no FMA ⇒ exact on every vendor — the moat the tensor tier cannot hold).
    int bad = 0;
    for (int i = 0; i < n_in; ++i) { if (out32[static_cast<crd::usize>(i)] != static_cast<float>(out64[static_cast<crd::usize>(i)])) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B14-c: CKIR SVGF a-trous denoiser DISPATCHES on Vulkan == CPU oracle (ULP-tol, transcendental weights)",
          "[gpu-context][vulkan][gpu][kernel][svgf]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::SvgfConfig            scfg; // 32×32
    kir::KGraph               g(&alloc);
    const kir::KEntry         e  = kir::build_svgf_atrous(g, scfg);
    const int                 np = scfg.width * scfg.height;

    // noisy flat surface (the meaningful denoise case)
    crd::containers::Array<crd::f64> color(&alloc);
    crd::containers::Array<crd::f64> gbuf(&alloc);
    crd::containers::Array<crd::f64> var(&alloc);
    crd::containers::Array<crd::f64> col_out(&alloc);
    crd::containers::Array<crd::f64> var_out(&alloc);
    color.resize(uz(np * 3));
    gbuf.resize(uz(np * 4));
    var.resize(uz(np));
    col_out.resize(uz(np * 3));
    var_out.resize(uz(np));
    crd::u32 s = 999U;
    auto rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < np; ++p)
    {
        for (int c = 0; c < 3; ++c) { color[uz(p * 3 + c)] = 0.4 + 0.3 * (rnd() - 0.5); }
        var[uz(p)]          = 0.05;
        gbuf[uz(p * 4 + 0)] = 1.0 + 0.1 * rnd();
        gbuf[uz(p * 4 + 1)] = 0.0;
        gbuf[uz(p * 4 + 2)] = 0.0;
        gbuf[uz(p * 4 + 3)] = 1.0;
    }
    kir::KernelBuffer bufs[5] = {{color.data(), np * 3, 0, 0}, {gbuf.data(), np * 4, 0, 1}, {var.data(), np, 0, 2},
                                 {col_out.data(), np * 3, 0, 3}, {var_out.data(), np, 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, e.local_size[0], &alloc, static_cast<crd::u32>(np / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_svgf", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> hc(&alloc);
    crd::containers::Array<float> hg(&alloc);
    crd::containers::Array<float> hv(&alloc);
    crd::containers::Array<float> hco(&alloc);
    crd::containers::Array<float> hvo(&alloc);
    hc.resize(uz(np * 3));
    hg.resize(uz(np * 4));
    hv.resize(uz(np));
    hco.resize(uz(np * 3));
    hvo.resize(uz(np));
    for (int i = 0; i < np * 3; ++i) { hc[uz(i)] = static_cast<float>(color[uz(i)]); }
    for (int i = 0; i < np * 4; ++i) { hg[uz(i)] = static_cast<float>(gbuf[uz(i)]); }
    for (int i = 0; i < np; ++i) { hv[uz(i)] = static_cast<float>(var[uz(i)]); }
    for (int i = 0; i < np * 3; ++i) { hco[uz(i)] = -9.0F; }
    float*    host[5] = {hc.data(), hg.data(), hv.data(), hco.data(), hvo.data()};
    const int lens[5] = {np * 3, np * 4, np, np * 3, np};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 5, static_cast<crd::u32>(np / 64));

    double maxrel = 0.0;
    for (int i = 0; i < np * 3; ++i)
    {
        const double ref = col_out[uz(i)];
        const double got = static_cast<double>(hco[uz(i)]);
        const double rel = std::fabs(got - ref) / (std::fabs(ref) + 1e-3);
        if (rel > maxrel) { maxrel = rel; }
    }
    std::printf("[Vulkan SVGF a-trous 32x32] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4); // arithmetic bit-exact; exp/pow weights ULP-tolerant (the B8 transcendental bar)
}

TEST_CASE("B14-b: CKIR DDGI probe sample (octahedral + Chebyshev + trilinear) DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][ddgi]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::ddgi::DdgiConfig     dcfg;
    const int                 r = dcfg.oct_res;
    kir::KGraph               g(&alloc);
    const kir::KEntry         e  = kir::ddgi::build_ddgi_sample(g, dcfg);
    const int                 nq = 64;

    // a non-uniform probe field (each probe a different colour + a partial occluder) so the blend/weights are exercised.
    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> nrm(&alloc);
    crd::containers::Array<crd::f64> irr(&alloc);
    crd::containers::Array<crd::f64> dpt(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    pos.resize(uz(nq * 3));
    nrm.resize(uz(nq * 3));
    irr.resize(uz(8 * r * r * 3));
    dpt.resize(uz(8 * r * r * 2));
    out.resize(uz(nq * 3));
    for (int pi = 0; pi < 8; ++pi)
    {
        for (int t = 0; t < r * r; ++t)
        {
            irr[uz(pi * r * r * 3 + t * 3 + 0)] = 0.1 * pi;
            irr[uz(pi * r * r * 3 + t * 3 + 1)] = 0.05 * t;
            irr[uz(pi * r * r * 3 + t * 3 + 2)] = 0.5;
            dpt[uz(pi * r * r * 2 + t * 2 + 0)] = (pi == 3) ? 0.05 : 50.0;
            dpt[uz(pi * r * r * 2 + t * 2 + 1)] = (pi == 3) ? 0.003 : 2500.0;
        }
    }
    crd::u32 s = 17U;
    auto rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < nq; ++p)
    {
        pos[uz(p * 3 + 0)] = rnd();
        pos[uz(p * 3 + 1)] = rnd();
        pos[uz(p * 3 + 2)] = rnd();
        const double nyv = rnd() * 2.0 - 1.0;
        nrm[uz(p * 3 + 0)] = 0.3;
        nrm[uz(p * 3 + 1)] = nyv;
        nrm[uz(p * 3 + 2)] = 0.6;
    }
    kir::KernelBuffer bufs[5] = {{pos.data(), nq * 3, 0, 0}, {nrm.data(), nq * 3, 0, 1}, {irr.data(), 8 * r * r * 3, 0, 2},
                                 {dpt.data(), 8 * r * r * 2, 0, 3}, {out.data(), nq * 3, 0, 4}};
    kir::eval_cpu_kernel(g, e, bufs, 5, e.local_size[0], &alloc, static_cast<crd::u32>(nq / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_ddgi", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 5, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[5] = {nq * 3, nq * 3, 8 * r * r * 3, 8 * r * r * 2, nq * 3};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    crd::containers::Array<float> h2(&alloc);
    crd::containers::Array<float> h3(&alloc);
    crd::containers::Array<float> h4(&alloc);
    crd::containers::Array<float>* h[5] = {&h0, &h1, &h2, &h3, &h4};
    float*                        host[5];
    for (int b = 0; b < 5; ++b) { h[b]->resize(uz(lens[b])); host[b] = h[b]->data(); }
    for (int b = 0; b < 4; ++b) { for (int i = 0; i < lens[b]; ++i) { (*h[b])[uz(i)] = static_cast<float>(bufs[b].data[i]); } }
    for (int i = 0; i < lens[4]; ++i) { h4[uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 5, static_cast<crd::u32>(nq / 64));

    double maxrel = 0.0;
    for (int i = 0; i < lens[4]; ++i)
    {
        const double ref = out[uz(i)];
        const double got = static_cast<double>(h4[uz(i)]);
        maxrel           = std::max(maxrel, std::fabs(got - ref) / (std::fabs(ref) + 1e-3));
    }
    std::printf("[Vulkan DDGI sample 8-probe] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4); // octahedral/Chebyshev/trilinear ⇒ arithmetic bit-exact, sqrt IEEE ⇒ effectively exact
}

TEST_CASE("B14-b: CKIR DDGI probe UPDATE (octahedral integrate + moments + hysteresis) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][ddgi]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::ddgi::DdgiConfig     dcfg;
    const int                 r  = dcfg.oct_res;
    const int                 nr = dcfg.num_rays;
    kir::KGraph               g(&alloc);
    const kir::KEntry         e   = kir::ddgi::build_ddgi_probe_update(g, dcfg);
    const int                 ntx = 8 * r * r;

    crd::containers::Array<crd::f64> b[7];
    const int                        lens[7] = {8 * nr * 3, 8 * nr * 3, 8 * nr, ntx * 3, ntx * 2, ntx * 3, ntx * 2};
    for (int k = 0; k < 7; ++k) { b[k] = crd::containers::Array<crd::f64>(&alloc); b[k].resize(uz(lens[k])); }
    crd::u32 s = 91U;
    auto rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < 8 * nr; ++i)
    {
        const double a = rnd() * 6.28;
        b[0][uz(i * 3 + 0)] = std::cos(a); b[0][uz(i * 3 + 1)] = 0.5; b[0][uz(i * 3 + 2)] = std::sin(a); // varied ray dirs
        b[1][uz(i * 3 + 0)] = 0.6; b[1][uz(i * 3 + 1)] = 0.3; b[1][uz(i * 3 + 2)] = 0.1;                 // radiance
        b[2][uz(i)]         = 2.0 + 4.0 * rnd();                                                          // hit distance
    }
    for (int i = 0; i < ntx * 3; ++i) { b[3][uz(i)] = 0.1 * rnd(); } // non-zero prev ⇒ the hysteresis lerp is exercised
    for (int i = 0; i < ntx * 2; ++i) { b[4][uz(i)] = 0.2 * rnd(); }
    kir::KernelBuffer kb[7];
    for (int k = 0; k < 7; ++k) { kb[k] = {b[k].data(), lens[k], 0, static_cast<crd::u8>(k)}; }
    kir::eval_cpu_kernel(g, e, kb, 7, e.local_size[0], &alloc, static_cast<crd::u32>(ntx / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_ddgi_upd", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 7, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> hf[7];
    float*                        host[7];
    for (int k = 0; k < 7; ++k) { hf[k] = crd::containers::Array<float>(&alloc); hf[k].resize(uz(lens[k])); host[k] = hf[k].data(); }
    for (int k = 0; k < 5; ++k) { for (int i = 0; i < lens[k]; ++i) { hf[k][uz(i)] = static_cast<float>(b[k].data()[i]); } }
    for (int i = 0; i < lens[5]; ++i) { hf[5][uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 7, static_cast<crd::u32>(ntx / 64));

    double maxrel = 0.0;
    for (int k = 5; k < 7; ++k)
    {
        for (int i = 0; i < lens[k]; ++i) { maxrel = std::max(maxrel, std::fabs(static_cast<double>(hf[k][uz(i)]) - b[k][uz(i)]) / (std::fabs(b[k][uz(i)]) + 1e-3)); }
    }
    std::printf("[Vulkan DDGI probe-update] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4); // integrate/moments/hysteresis + pow (cos^sharpness) ULP ⇒ effectively exact
}

TEST_CASE("B14-a: CKIR ReSTIR RIS (weighted reservoir resampling) DISPATCHES on Vulkan == CPU oracle (bit-exact)",
          "[gpu-context][vulkan][gpu][kernel][restir]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::restir::RestirConfig  rcfg;
    const int                  m = rcfg.num_candidates;
    const int                  n = 256;
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::restir::build_restir_ris(g, rcfg);

    // a stream of light candidates per pixel: (f = contribution, p̂ = target, xi = the WRS random). Varied p̂≠f so the
    // WRS keep-probability (div by the running Σw) and the final W = Σw/(M·p̂) are both genuinely exercised.
    crd::containers::Array<crd::f64> cand(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    cand.resize(uz(n * m * 3));
    out.resize(uz(n * 4));
    crd::u32 s   = 4242U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < n; ++p)
    {
        for (int i = 0; i < m; ++i)
        {
            const int base     = (p * m + i) * 3;
            cand[uz(base + 0)] = 0.2 + 2.0 * rnd();  // f
            cand[uz(base + 1)] = 0.1 + 1.5 * rnd();  // p̂ (independent of f ⇒ imperfect target)
            cand[uz(base + 2)] = rnd();              // WRS random
        }
    }
    kir::KernelBuffer bufs[2] = {{cand.data(), n * m * 3, 0, 0}, {out.data(), n * 4, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_restir_ris", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {n * m * 3, n * 4};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    crd::containers::Array<float>* h[2] = {&h0, &h1};
    float*                        host[2];
    for (int b = 0; b < 2; ++b) { h[b]->resize(uz(lens[b])); host[b] = h[b]->data(); }
    for (int i = 0; i < lens[0]; ++i) { h0[uz(i)] = static_cast<float>(bufs[0].data[i]); }
    for (int i = 0; i < lens[1]; ++i) { h1[uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxrel = 0.0;
    for (int i = 0; i < lens[1]; ++i)
    {
        const double ref = out[uz(i)];
        maxrel           = std::max(maxrel, std::fabs(static_cast<double>(h1[uz(i)]) - ref) / (std::fabs(ref) + 1e-3));
    }
    std::printf("[Vulkan ReSTIR RIS] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4); // add/mul/div/cmp/select only — div is IEEE-exact ⇒ effectively bit-exact
}

TEST_CASE("B14-a: CKIR ReSTIR TEMPORAL merge (two-reservoir combine + M clamp) DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][restir]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::restir::RestirConfig  rcfg;
    const int                  n = 256;
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::restir::build_restir_temporal(g, rcfg);

    // cur + prev reservoirs [f, p̂, W, M] with a HIGH prev M so the m_cap clamp (prev.M -> min(prev.M, m_cap·cur.M)) engages.
    crd::containers::Array<crd::f64> cur(&alloc);
    crd::containers::Array<crd::f64> prev(&alloc);
    crd::containers::Array<crd::f64> xi(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    cur.resize(uz(n * 4));
    prev.resize(uz(n * 4));
    xi.resize(uz(n));
    out.resize(uz(n * 4));
    crd::u32 s   = 71U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < n; ++p)
    {
        cur[uz(p * 4 + 0)]  = 0.2 + 2.0 * rnd(); cur[uz(p * 4 + 1)]  = 0.1 + 1.5 * rnd();
        cur[uz(p * 4 + 2)]  = 0.3 + rnd();       cur[uz(p * 4 + 3)]  = 32.0;
        prev[uz(p * 4 + 0)] = 0.2 + 2.0 * rnd(); prev[uz(p * 4 + 1)] = 0.1 + 1.5 * rnd();
        prev[uz(p * 4 + 2)] = 0.3 + rnd();       prev[uz(p * 4 + 3)] = 400.0 + 800.0 * rnd(); // > m_cap·32 for some ⇒ clamp
        xi[uz(p)]           = rnd();
    }
    kir::KernelBuffer bufs[4] = {{cur.data(), n * 4, 0, 0}, {prev.data(), n * 4, 0, 1}, {xi.data(), n, 0, 2}, {out.data(), n * 4, 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_restir_tmp", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 4, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[4] = {n * 4, n * 4, n, n * 4};
    crd::containers::Array<float> hf[4];
    float*                        host[4];
    for (int k = 0; k < 4; ++k) { hf[k] = crd::containers::Array<float>(&alloc); hf[k].resize(uz(lens[k])); host[k] = hf[k].data(); }
    for (int k = 0; k < 3; ++k) { for (int i = 0; i < lens[k]; ++i) { hf[k][uz(i)] = static_cast<float>(bufs[k].data[i]); } }
    for (int i = 0; i < lens[3]; ++i) { hf[3][uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 4, static_cast<crd::u32>(n / 64));

    double maxrel = 0.0;
    for (int i = 0; i < lens[3]; ++i)
    {
        const double ref = out[uz(i)];
        maxrel           = std::max(maxrel, std::fabs(static_cast<double>(hf[3][uz(i)]) - ref) / (std::fabs(ref) + 1e-3));
    }
    std::printf("[Vulkan ReSTIR temporal] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4); // add/mul/div/min/max/cmp/select only ⇒ effectively bit-exact
}

TEST_CASE("B14-a: CKIR ReSTIR SPATIAL reuse (K-neighbour reservoir merge) DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][restir]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::restir::RestirConfig  rcfg;
    const int                  k = rcfg.spatial_neighbors;
    const int                  n = 256;
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::restir::build_restir_spatial(g, rcfg);

    // N reservoirs [f, p̂, W, M]; each pixel pulls K neighbour reservoir indices + K WRS randoms.
    crd::containers::Array<crd::f64> res(&alloc);
    crd::containers::Array<crd::f64> nbr(&alloc);
    crd::containers::Array<crd::f64> xi(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    res.resize(uz(n * 4));
    nbr.resize(uz(n * k));
    xi.resize(uz(n * k));
    out.resize(uz(n * 4));
    crd::u32 s   = 55U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < n; ++p)
    {
        res[uz(p * 4 + 0)] = 0.2 + 2.0 * rnd(); res[uz(p * 4 + 1)] = 0.1 + 1.5 * rnd();
        res[uz(p * 4 + 2)] = 0.3 + rnd();       res[uz(p * 4 + 3)] = 16.0 + 48.0 * rnd();
        for (int j = 0; j < k; ++j) { nbr[uz(p * k + j)] = static_cast<double>(static_cast<int>(rnd() * n) % n); xi[uz(p * k + j)] = rnd(); }
    }
    kir::KernelBuffer bufs[4] = {{res.data(), n * 4, 0, 0}, {nbr.data(), n * k, 0, 1}, {xi.data(), n * k, 0, 2}, {out.data(), n * 4, 0, 3}};
    kir::eval_cpu_kernel(g, e, bufs, 4, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_restir_sp", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 4, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[4] = {n * 4, n * k, n * k, n * 4};
    crd::containers::Array<float> hf[4];
    float*                        host[4];
    for (int b = 0; b < 4; ++b) { hf[b] = crd::containers::Array<float>(&alloc); hf[b].resize(uz(lens[b])); host[b] = hf[b].data(); }
    for (int b = 0; b < 3; ++b) { for (int i = 0; i < lens[b]; ++i) { hf[b][uz(i)] = static_cast<float>(bufs[b].data[i]); } }
    for (int i = 0; i < lens[3]; ++i) { hf[3][uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 4, static_cast<crd::u32>(n / 64));

    double maxrel = 0.0;
    for (int i = 0; i < lens[3]; ++i)
    {
        const double ref = out[uz(i)];
        maxrel           = std::max(maxrel, std::fabs(static_cast<double>(hf[3][uz(i)]) - ref) / (std::fabs(ref) + 1e-3));
    }
    std::printf("[Vulkan ReSTIR spatial] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 1e-4); // add/mul/div/max/cmp/select + a cast-to-index only ⇒ effectively bit-exact
}

TEST_CASE("B15-a: CKIR atmosphere TRANSMITTANCE LUT (Hillaire/Bruneton extinction integral) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][atmos]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator   alloc(64U << 20U);
    kir::atmos::AtmosphereConfig acfg;
    const int                    w   = acfg.tlut_w;
    const int                    ht  = acfg.tlut_h;
    const int                    nel = w * ht * 3;
    kir::KGraph                  g(&alloc);
    const kir::KEntry            e = kir::atmos::build_atmos_transmittance(g, acfg);

    crd::containers::Array<crd::f64> out(&alloc);
    out.resize(uz(nel));
    kir::KernelBuffer bufs[1] = {{out.data(), nel, 0, 0}};
    kir::eval_cpu_kernel(g, e, bufs, 1, e.local_size[0], &alloc, static_cast<crd::u32>(w * ht / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_atmos_tlut", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 1, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[1] = {nel};
    crd::containers::Array<float> h0(&alloc);
    h0.resize(uz(nel));
    for (int i = 0; i < nel; ++i) { h0[uz(i)] = -9.0F; }
    float* host[1] = {h0.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 1, static_cast<crd::u32>(w * ht / 64));

    double maxabs = 0.0;
    for (int i = 0; i < nel; ++i)
    {
        maxabs = std::max(maxabs, std::fabs(static_cast<double>(h0[uz(i)]) - out[uz(i)]));
    }
    std::printf("[Vulkan atmosphere transmittance LUT] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    // T ∈ [0,1] down to ~0 at the horizon ⇒ ABSOLUTE error is the physical metric (relative would divide by a near-zero ref).
    // Only exp/sqrt diverge (GPU-hardware vs CPU-libm, ~3 ULP each); over a 40-step exp accumulation with km-scale radii the
    // floor is ~6e-5 measured — 0.006% of a [0,1] transmittance, at the hardware-transcendental limit (no logic divergence:
    // add/sub/mul/div/max/min are all bit-exact; the μ mapping is cancellation-free). Threshold has driver/GPU headroom.
    CHECK(maxabs < 2e-4);
}

TEST_CASE("B15-a: CKIR atmosphere MULTIPLE-SCATTERING LUT (Hillaire isotropic series) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][atmos]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator   alloc(128U << 20U);
    kir::atmos::AtmosphereConfig acfg;
    const int                    tw  = acfg.tlut_w;
    const int                    th  = acfg.tlut_h;
    const int                    res = acfg.mslut_res;
    const int                    tn  = tw * th * 3;
    const int                    mn  = res * res * 3;

    // the transmittance LUT (the multiscatter kernel's INPUT). Round it to f32 so the CPU oracle and the GPU sample IDENTICAL
    // input values — the only divergence we then measure is the multiscatter kernel's own exp/sqrt/bilinear arithmetic.
    kir::KGraph       gt(&alloc);
    const kir::KEntry et = kir::atmos::build_atmos_transmittance(gt, acfg);
    crd::containers::Array<crd::f64> tlut(&alloc);
    tlut.resize(uz(tn));
    kir::KernelBuffer tb[1] = {{tlut.data(), tn, 0, 0}};
    kir::eval_cpu_kernel(gt, et, tb, 1, et.local_size[0], &alloc, static_cast<crd::u32>(tw * th / 64));
    for (int i = 0; i < tn; ++i) { tlut[uz(i)] = static_cast<double>(static_cast<float>(tlut[uz(i)])); }

    kir::KGraph       gm(&alloc);
    const kir::KEntry em = kir::atmos::build_atmos_multiscatter(gm, acfg);
    crd::containers::Array<crd::f64> ms(&alloc);
    ms.resize(uz(mn));
    kir::KernelBuffer mb[2] = {{tlut.data(), tn, 0, 0}, {ms.data(), mn, 0, 1}};
    kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(res * res / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(gm, em, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_atmos_ms", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {tn, mn};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(tn));
    h1.resize(uz(mn));
    for (int i = 0; i < tn; ++i) { h0[uz(i)] = static_cast<float>(tlut[uz(i)]); }
    for (int i = 0; i < mn; ++i) { h1[uz(i)] = -9.0F; }
    float* host[2] = {h0.data(), h1.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(res * res / 64));

    double maxabs = 0.0;
    for (int i = 0; i < mn; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h1[uz(i)]) - ms[uz(i)])); }
    std::printf("[Vulkan atmosphere multiscatter LUT] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    // Ψ is a small fill radiance; only exp/sqrt (GPU vs libm) diverge over the sphere×march accumulation + bilinear LUT taps.
    CHECK(maxabs < 2e-4);
}

TEST_CASE("B15-a: CKIR atmosphere SKY-VIEW LUT (single+multiple scattering, both LUTs sampled) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][atmos]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator   alloc(128U << 20U);
    kir::atmos::AtmosphereConfig acfg;
    acfg.tlut_w = 64; // reduced input LUTs so the CPU-oracle reference stays fast; the GPU kernel is per-texel identical.
    acfg.tlut_h = 16;
    acfg.mslut_res = 16;
    const int tw = acfg.tlut_w;
    const int th = acfg.tlut_h;
    const int rs = acfg.mslut_res;
    const int sw = acfg.skyview_w;
    const int sh = acfg.skyview_h;
    const int tn = tw * th * 3;
    const int rn = rs * rs * 3;
    const int sn = sw * sh * 3;

    // transmittance LUT → f32-round → multiscatter LUT → f32-round, so the CPU oracle and the GPU sky-view sample IDENTICAL
    // input textures; the only measured divergence is then the sky-view kernel's own exp/sqrt/bilinear/phase arithmetic.
    kir::KGraph       gt(&alloc);
    const kir::KEntry et = kir::atmos::build_atmos_transmittance(gt, acfg);
    crd::containers::Array<crd::f64> tlut(&alloc);
    tlut.resize(uz(tn));
    kir::KernelBuffer tb[1] = {{tlut.data(), tn, 0, 0}};
    kir::eval_cpu_kernel(gt, et, tb, 1, et.local_size[0], &alloc, static_cast<crd::u32>(tw * th / 64));
    for (int i = 0; i < tn; ++i) { tlut[uz(i)] = static_cast<double>(static_cast<float>(tlut[uz(i)])); }

    kir::KGraph       gm(&alloc);
    const kir::KEntry em = kir::atmos::build_atmos_multiscatter(gm, acfg);
    crd::containers::Array<crd::f64> ms(&alloc);
    ms.resize(uz(rn));
    kir::KernelBuffer mb[2] = {{tlut.data(), tn, 0, 0}, {ms.data(), rn, 0, 1}};
    kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(rs * rs / 64));
    for (int i = 0; i < rn; ++i) { ms[uz(i)] = static_cast<double>(static_cast<float>(ms[uz(i)])); }

    kir::KGraph       gs(&alloc);
    const kir::KEntry es = kir::atmos::build_atmos_skyview(gs, acfg);
    crd::containers::Array<crd::f64> sv(&alloc);
    sv.resize(uz(sn));
    kir::KernelBuffer sb[3] = {{tlut.data(), tn, 0, 0}, {ms.data(), rn, 0, 1}, {sv.data(), sn, 0, 2}};
    kir::eval_cpu_kernel(gs, es, sb, 3, es.local_size[0], &alloc, static_cast<crd::u32>(sw * sh / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(gs, es, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_atmos_sky", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[3] = {tn, rn, sn};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    crd::containers::Array<float> h2(&alloc);
    h0.resize(uz(tn));
    h1.resize(uz(rn));
    h2.resize(uz(sn));
    for (int i = 0; i < tn; ++i) { h0[uz(i)] = static_cast<float>(tlut[uz(i)]); }
    for (int i = 0; i < rn; ++i) { h1[uz(i)] = static_cast<float>(ms[uz(i)]); }
    for (int i = 0; i < sn; ++i) { h2[uz(i)] = -9.0F; }
    float* host[3] = {h0.data(), h1.data(), h2.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 3, static_cast<crd::u32>(sw * sh / 64));

    double maxabs = 0.0;
    for (int i = 0; i < sn; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h2[uz(i)]) - sv[uz(i)])); }
    std::printf("[Vulkan atmosphere sky-view LUT] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    // sky radiance from single (phase-weighted) + multiple scattering; only exp/sqrt/pow (GPU vs libm) diverge over the march.
    CHECK(maxabs < 5e-4);
}

TEST_CASE("B15-a: CKIR atmosphere AERIAL-PERSPECTIVE froxels (3D volume, both LUTs) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][atmos]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator   alloc(128U << 20U);
    kir::atmos::AtmosphereConfig acfg;
    acfg.tlut_w = 64;
    acfg.tlut_h = 16;
    acfg.mslut_res = 16;
    const int tw = acfg.tlut_w;
    const int th = acfg.tlut_h;
    const int rr = acfg.mslut_res;
    const int apr = acfg.ap_res;
    const int aps = acfg.ap_slices;
    const int tn = tw * th * 3;
    const int rn = rr * rr * 3;
    const int an = apr * apr * aps * 4;

    kir::KGraph       gt(&alloc);
    const kir::KEntry et = kir::atmos::build_atmos_transmittance(gt, acfg);
    crd::containers::Array<crd::f64> tlut(&alloc);
    tlut.resize(uz(tn));
    kir::KernelBuffer tb[1] = {{tlut.data(), tn, 0, 0}};
    kir::eval_cpu_kernel(gt, et, tb, 1, et.local_size[0], &alloc, static_cast<crd::u32>(tw * th / 64));
    for (int i = 0; i < tn; ++i) { tlut[uz(i)] = static_cast<double>(static_cast<float>(tlut[uz(i)])); }

    kir::KGraph       gm(&alloc);
    const kir::KEntry em = kir::atmos::build_atmos_multiscatter(gm, acfg);
    crd::containers::Array<crd::f64> ms(&alloc);
    ms.resize(uz(rn));
    kir::KernelBuffer mb[2] = {{tlut.data(), tn, 0, 0}, {ms.data(), rn, 0, 1}};
    kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(rr * rr / 64));
    for (int i = 0; i < rn; ++i) { ms[uz(i)] = static_cast<double>(static_cast<float>(ms[uz(i)])); }

    kir::KGraph       ga(&alloc);
    const kir::KEntry ea = kir::atmos::build_atmos_aerial(ga, acfg);
    crd::containers::Array<crd::f64> ap(&alloc);
    ap.resize(uz(an));
    kir::KernelBuffer ab[3] = {{tlut.data(), tn, 0, 0}, {ms.data(), rn, 0, 1}, {ap.data(), an, 0, 2}};
    kir::eval_cpu_kernel(ga, ea, ab, 3, ea.local_size[0], &alloc, static_cast<crd::u32>(apr * apr / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(ga, ea, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_atmos_ap", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[3] = {tn, rn, an};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    crd::containers::Array<float> h2(&alloc);
    h0.resize(uz(tn));
    h1.resize(uz(rn));
    h2.resize(uz(an));
    for (int i = 0; i < tn; ++i) { h0[uz(i)] = static_cast<float>(tlut[uz(i)]); }
    for (int i = 0; i < rn; ++i) { h1[uz(i)] = static_cast<float>(ms[uz(i)]); }
    for (int i = 0; i < an; ++i) { h2[uz(i)] = -9.0F; }
    float* host[3] = {h0.data(), h1.data(), h2.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 3, static_cast<crd::u32>(apr * apr / 64));

    double maxabs = 0.0;
    for (int i = 0; i < an; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h2[uz(i)]) - ap[uz(i)])); }
    std::printf("[Vulkan atmosphere aerial-perspective] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    CHECK(maxabs < 5e-4);
}

TEST_CASE("B14-d: CKIR NRC hash-grid ENCODER (Instant-NGP, trilinear hashed features) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][nrc]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::nrc::NrcConfig        ncfg;
    const int                  n  = 256;
    const int                  lf = ncfg.encoded_dim();
    const int                  ts = ncfg.levels * ncfg.table_size * ncfg.features;
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::nrc::build_nrc_hashgrid_encode(g, ncfg);

    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> tab(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    pos.resize(uz(n * 3));
    tab.resize(uz(ts));
    out.resize(uz(n * lf));
    crd::u32 s   = 7U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n * 3; ++i) { pos[uz(i)] = rnd(); }
    for (int i = 0; i < ts; ++i) { tab[uz(i)] = rnd() * 2.0 - 1.0; } // learnable features (signed)
    kir::KernelBuffer bufs[3] = {{pos.data(), n * 3, 0, 0}, {tab.data(), ts, 0, 1}, {out.data(), n * lf, 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_nrc_enc", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[3] = {n * 3, ts, n * lf};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    crd::containers::Array<float> h2(&alloc);
    h0.resize(uz(n * 3));
    h1.resize(uz(ts));
    h2.resize(uz(n * lf));
    for (int i = 0; i < n * 3; ++i) { h0[uz(i)] = static_cast<float>(pos[uz(i)]); }
    for (int i = 0; i < ts; ++i) { h1[uz(i)] = static_cast<float>(tab[uz(i)]); }
    for (int i = 0; i < n * lf; ++i) { h2[uz(i)] = -9.0F; }
    float* host[3] = {h0.data(), h1.data(), h2.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 3, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    for (int i = 0; i < n * lf; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h2[uz(i)]) - out[uz(i)])); }
    std::printf("[Vulkan NRC hash-grid encode] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    CHECK(maxabs < 1e-6); // integer spatial hash + trilinear blend — no transcendentals ⇒ bit-exact (f32 sum ULP only)
}

TEST_CASE("B14-d: CKIR NRC MLP inference + TRAINING backprop DISPATCH on Vulkan == CPU oracle (bit-exact)",
          "[gpu-context][vulkan][gpu][kernel][nrc]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::nrc::NrcConfig        ncfg;
    const int                  d = ncfg.encoded_dim();
    const int                  h = ncfg.hidden;
    const int                  o = ncfg.out_dim;
    const int                  n = 256;
    crd::u32 s   = 33U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24) * 2.0 - 1.0; };

    crd::containers::Array<crd::f64> enc(&alloc);
    crd::containers::Array<crd::f64> w1(&alloc);
    crd::containers::Array<crd::f64> b1(&alloc);
    crd::containers::Array<crd::f64> w2(&alloc);
    crd::containers::Array<crd::f64> b2(&alloc);
    enc.resize(uz(n * d)); w1.resize(uz(h * d)); b1.resize(uz(h)); w2.resize(uz(o * h)); b2.resize(uz(o));
    for (int i = 0; i < n * d; ++i) { enc[uz(i)] = rnd(); }
    for (int i = 0; i < h * d; ++i) { w1[uz(i)] = rnd() * 0.5; }
    for (int i = 0; i < h; ++i) { b1[uz(i)] = rnd() * 0.2; }
    for (int i = 0; i < o * h; ++i) { w2[uz(i)] = rnd() * 0.5; }
    for (int i = 0; i < o; ++i) { b2[uz(i)] = rnd() * 0.1; }

    // ── inference ──
    {
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::nrc::build_nrc_infer(g, ncfg);
        crd::containers::Array<crd::f64> out(&alloc);
        out.resize(uz(n * o));
        kir::KernelBuffer bufs[6] = {{enc.data(), n * d, 0, 0}, {w1.data(), h * d, 0, 1}, {b1.data(), h, 0, 2}, {w2.data(), o * h, 0, 3}, {b2.data(), o, 0, 4}, {out.data(), n * o, 0, 5}};
        kir::eval_cpu_kernel(g, e, bufs, 6, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_nrc_infer", &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 6, 0U);
        REQUIRE(pipe != nullptr);
        const int lens[6] = {n * d, h * d, h, o * h, o, n * o};
        crd::containers::Array<float> hb[6];
        float*                        host[6];
        for (int k = 0; k < 6; ++k) { hb[k] = crd::containers::Array<float>(&alloc); hb[k].resize(uz(lens[k])); host[k] = hb[k].data(); }
        for (int k = 0; k < 5; ++k) { for (int i = 0; i < lens[k]; ++i) { hb[k][uz(i)] = static_cast<float>(bufs[k].data[i]); } }
        for (int i = 0; i < lens[5]; ++i) { hb[5][uz(i)] = -9.0F; }
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 6, static_cast<crd::u32>(n / 64));
        double mdiff = 0.0;
        for (int i = 0; i < n * o; ++i) { mdiff = std::max(mdiff, std::fabs(static_cast<double>(hb[5][uz(i)]) - out[uz(i)])); }
        std::printf("[Vulkan NRC infer] maxabs = %.2e\n", mdiff);
        CHECK(mdiff < 1e-5);
    }
    // ── training backprop ──
    {
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::nrc::build_nrc_train_grad(g, ncfg);
        crd::containers::Array<crd::f64> tgt(&alloc);
        crd::containers::Array<crd::f64> gw1(&alloc);
        crd::containers::Array<crd::f64> gw2(&alloc);
        tgt.resize(uz(n * o)); gw1.resize(uz(n * h * d)); gw2.resize(uz(n * o * h));
        for (int i = 0; i < n * o; ++i) { tgt[uz(i)] = rnd(); }
        kir::KernelBuffer bufs[8] = {{enc.data(), n * d, 0, 0}, {w1.data(), h * d, 0, 1}, {b1.data(), h, 0, 2}, {w2.data(), o * h, 0, 3}, {b2.data(), o, 0, 4}, {tgt.data(), n * o, 0, 5}, {gw1.data(), n * h * d, 0, 6}, {gw2.data(), n * o * h, 0, 7}};
        kir::eval_cpu_kernel(g, e, bufs, 8, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_nrc_train", &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 8, 0U);
        REQUIRE(pipe != nullptr);
        const int lens[8] = {n * d, h * d, h, o * h, o, n * o, n * h * d, n * o * h};
        crd::containers::Array<float> hb[8];
        float*                        host[8];
        for (int k = 0; k < 8; ++k) { hb[k] = crd::containers::Array<float>(&alloc); hb[k].resize(uz(lens[k])); host[k] = hb[k].data(); }
        for (int k = 0; k < 6; ++k) { for (int i = 0; i < lens[k]; ++i) { hb[k][uz(i)] = static_cast<float>(bufs[k].data[i]); } }
        for (int i = 0; i < lens[6]; ++i) { hb[6][uz(i)] = -9.0F; }
        for (int i = 0; i < lens[7]; ++i) { hb[7][uz(i)] = -9.0F; }
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 8, static_cast<crd::u32>(n / 64));
        double mdiff = 0.0;
        for (int i = 0; i < n * h * d; ++i) { mdiff = std::max(mdiff, std::fabs(static_cast<double>(hb[6][uz(i)]) - gw1[uz(i)])); }
        for (int i = 0; i < n * o * h; ++i) { mdiff = std::max(mdiff, std::fabs(static_cast<double>(hb[7][uz(i)]) - gw2[uz(i)])); }
        std::printf("[Vulkan NRC train-grad] maxabs = %.2e\n", mdiff);
        CHECK(mdiff < 1e-5);
    }
}

TEST_CASE("B15-b: CKIR cloud DENSITY field (Nubis Perlin-Worley + coverage + erosion) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][clouds]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::clouds::CloudConfig   ccfg;
    const int                  n = 256;
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::clouds::build_cloud_density(g, ccfg);

    crd::containers::Array<crd::f64> pos(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    pos.resize(uz(n * 3));
    out.resize(uz(n));
    crd::u32 s   = 88U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        pos[uz(i * 3 + 0)] = rnd() * 6.0;
        pos[uz(i * 3 + 1)] = rnd() * 6.0;
        pos[uz(i * 3 + 2)] = ccfg.cloud_base + rnd() * (ccfg.cloud_top - ccfg.cloud_base);
    }
    kir::KernelBuffer bufs[2] = {{pos.data(), n * 3, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    // optimize=TRUE: the Perlin-Worley noise is a compact RUNTIME loop (worley3_loop collapses the 27-cell unroll), so spirv-opt
    // is fast + the pipeline compiles in seconds — the gold procedural cloud shape runs fully optimized (no baked-texture crutch).
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_cloud_density", &alloc, true);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {n * 3, n};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 3));
    h1.resize(uz(n));
    for (int i = 0; i < n * 3; ++i) { h0[uz(i)] = static_cast<float>(pos[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float* host[2] = {h0.data(), h1.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    for (int i = 0; i < n; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h1[uz(i)]) - out[uz(i)])); }
    std::printf("[Vulkan cloud density] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    CHECK(maxabs < 1e-6); // Perlin/Worley noise = integer hash + lerp, no transcendentals ⇒ bit-exact
}

// B18-a: the Chiang R/TT/TRT/TRRT hair/fur BCSDF (Bessel-I0 longitudinal Mp, trimmed-logistic azimuthal Np, Fresnel/Beer
// attenuations Ap) DISPATCHES on Vulkan and the device output matches the CPU oracle to ULP. This is the PORTABILITY gate
// (physical energy conservation is certified separately by the CPU white-furnace test in tests/kir/test_ckir_hair.cpp). The
// BCSDF is transcendental-heavy (exp/log/asin/sinh), so this is a to-ULP tier like the atmosphere/MBOIT gates, not bit-exact.
TEST_CASE("B18-a: CKIR hair BCSDF (Chiang R/TT/TRT/TRRT) DISPATCHES on Vulkan == CPU oracle (to-ULP)",
          "[gpu-context][vulkan][gpu][kernel][hair]")
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
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const double kpi = kir::hair::kPi;

    crd::memory::TlsfAllocator  alloc(64U << 20U);
    kir::hair::HairKernelConfig hcfg; // η=1.55, βₘ=βₙ=0.3, α=2° — moderate roughness (all longitudinal variances > 0.1)
    const int                   n = 256;
    kir::KGraph                 g(&alloc);
    const kir::KEntry           e = kir::hair::build_hair_bcsdf_kernel(g, hcfg);

    // Random (sinθo, φo, sinθi, φi, h, σₐ) per lane spanning the fibre sphere; sinθ ∈ (−0.99, 0.99), φ ∈ (−π, π), h ∈ [−1,1].
    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 6));
    out.resize(uz(n));
    crd::u32 s   = 1337U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        in[uz(i * 6 + 0)] = (rnd() * 2.0 - 1.0) * 0.99; // sinθo
        in[uz(i * 6 + 1)] = (rnd() * 2.0 - 1.0) * kpi;  // φo
        in[uz(i * 6 + 2)] = (rnd() * 2.0 - 1.0) * 0.99; // sinθi
        in[uz(i * 6 + 3)] = (rnd() * 2.0 - 1.0) * kpi;  // φi
        in[uz(i * 6 + 4)] = rnd() * 2.0 - 1.0;          // h
        in[uz(i * 6 + 5)] = rnd() * 1.5;                // σₐ (monochrome)
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 6, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_hair_bcsdf", &alloc, true);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {n * 6, n};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 6));
    h1.resize(uz(n));
    for (int i = 0; i < n * 6; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float* host[2] = {h0.data(), h1.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    double maxrel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double gv = static_cast<double>(h1[uz(i)]);
        const double ov = out[uz(i)];
        const double ad = std::fabs(gv - ov);
        maxabs          = std::max(maxabs, ad);
        if (std::fabs(ov) > 1.0e-3) { maxrel = std::max(maxrel, ad / std::fabs(ov)); }
    }
    std::printf("[Vulkan hair BCSDF] maxabs(GPU vs oracle) = %.3e  maxrel = %.3e\n", maxabs, maxrel);
    // to-ULP: observed maxabs ~2e-7 (≈f32 bit-exact), maxrel ~7e-6 (tens of f32 ULP over the exp/log/asin/sinh/logistic chain).
    // A real transcription bug would be maxabs/maxrel ~O(0.1); these bounds keep margin for cross-driver transcendental variance.
    // ⛔ REN-38 llvmpipe campaign: this is the NATIVE-op tier, so the bound is the CONFORMANCE ENVELOPE, not
    // NVIDIA's delivered precision. The BCSDF cone runs exp(v) at |v| up to ~40 (longitudinal Gaussians) and the
    // Vulkan spec only guarantees sin/cos/exp to ~1e-4-class relative error — exp's condition number |v| then
    // amplifies a conformant argument error to ~4e-3 relative, and llvmpipe MEASURES maxrel 1.2e-2 (NV: 3e-5).
    // 3e-2 covers every conformant implementation while still catching a wrong lobe (orders of magnitude off).
    // The BIT-EXACT claims live in the deterministic-tier tests, not here.
    CHECK(maxabs < 1.0e-2);
    CHECK(maxrel < 3.0e-2);
}

// B18-b: the FUR BCSDF (hair R/TT/TRT/TRRT + the Yan-2017 double-cylinder MEDULLA scattered lobe: wrapped-Cauchy azimuthal +
// broad longitudinal, energy-split from TT/TRT) DISPATCHES on Vulkan == CPU oracle to-ULP. Same kernel path as B18-a with κ>0,
// so it exercises the full medulla branch (exp/cos/wrapped-Cauchy division) on the GPU.
TEST_CASE("B18-b: CKIR fur BCSDF (medulla double-cylinder) DISPATCHES on Vulkan == CPU oracle (to-ULP)",
          "[gpu-context][vulkan][gpu][kernel][hair][fur]")
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
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const double kpi = kir::hair::kPi;

    crd::memory::TlsfAllocator  alloc(64U << 20U);
    kir::hair::HairKernelConfig hcfg;
    hcfg.fur_kappa  = 0.6; // medullary index — a medulla filling 60% of the fibre radius
    hcfg.fur_sigma  = 3.0; // medulla extinction
    hcfg.fur_albedo = 0.7; // single-scatter albedo
    hcfg.fur_g      = 0.3; // forward-scattering anisotropy
    const int         n = 256;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hair::build_hair_bcsdf_kernel(g, hcfg);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 6));
    out.resize(uz(n));
    crd::u32 s   = 2027U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        in[uz(i * 6 + 0)] = (rnd() * 2.0 - 1.0) * 0.99;
        in[uz(i * 6 + 1)] = (rnd() * 2.0 - 1.0) * kpi;
        in[uz(i * 6 + 2)] = (rnd() * 2.0 - 1.0) * 0.99;
        in[uz(i * 6 + 3)] = (rnd() * 2.0 - 1.0) * kpi;
        in[uz(i * 6 + 4)] = rnd() * 2.0 - 1.0;
        in[uz(i * 6 + 5)] = rnd() * 1.5;
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 6, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_fur_bcsdf", &alloc, true);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {n * 6, n};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 6));
    h1.resize(uz(n));
    for (int i = 0; i < n * 6; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float* host[2] = {h0.data(), h1.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    double maxrel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double gv = static_cast<double>(h1[uz(i)]);
        const double ov = out[uz(i)];
        const double ad = std::fabs(gv - ov);
        maxabs          = std::max(maxabs, ad);
        if (std::fabs(ov) > 1.0e-3) { maxrel = std::max(maxrel, ad / std::fabs(ov)); }
    }
    std::printf("[Vulkan fur BCSDF] maxabs(GPU vs oracle) = %.3e  maxrel = %.3e\n", maxabs, maxrel);
    CHECK(maxabs < 1.0e-2); // same conformance envelope as the hair BCSDF above (same exp-amplified cone)
    CHECK(maxrel < 3.0e-2);
}

// B18-c: every hair MULTIPLE-SCATTERING tier DISPATCHES on Vulkan and matches the CPU oracle. One test covers all four kernels
// because they form a pipeline: the moment LUT feeds dual scattering, and the deep-opacity map supplies the strand count that
// dual scattering consumes. The physics of each is gated on the CPU side (tests/kir/test_ckir_hair_scatter.cpp); THIS gate is
// purely about portability — the same CKIR graphs lowering and running identically on the device.
TEST_CASE("B18-c: hair multiple-scattering tiers DISPATCH on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][hair][scatter]")
{
    namespace kir  = crd::kir;
    namespace hms  = crd::kir::hairms;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };
    crd::memory::TlsfAllocator alloc(192U << 20U);

    // Run a kernel through the CPU oracle AND the GPU from the SAME inputs, then report max|Δ| on buffer `check`.
    // The oracle writes outputs in place, so the inputs are snapshotted first and restored before the device run.
    const auto both = [&](kir::KGraph& g, const kir::KEntry& e, double** data, const int* lens, int nbuf, int check,
                          crd::u32 groups, const char* name) -> double {
        crd::containers::Array<double> snap(&alloc);
        int total = 0;
        for (int i = 0; i < nbuf; ++i) { total += lens[i]; }
        snap.resize(uz(total), 0.0);
        int off = 0;
        for (int i = 0; i < nbuf; ++i) { for (int j = 0; j < lens[i]; ++j) { snap[uz(off + j)] = data[i][j]; } off += lens[i]; }

        kir::KernelBuffer bufs[6];
        for (int i = 0; i < nbuf; ++i) { bufs[i] = {data[i], lens[i], 0U, static_cast<crd::u8>(i)}; } // binding is u8
        kir::eval_cpu_kernel(g, e, bufs, nbuf, e.local_size[0], &alloc, groups);
        crd::containers::Array<double> ref(&alloc);
        ref.resize(uz(lens[check]), 0.0);
        for (int j = 0; j < lens[check]; ++j) { ref[uz(j)] = data[check][j]; }

        off = 0; // restore the pre-oracle state for the device run
        for (int i = 0; i < nbuf; ++i) { for (int j = 0; j < lens[i]; ++j) { data[i][j] = snap[uz(off + j)]; } off += lens[i]; }

        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), name, &alloc, false);
        INFO("GLSL compile [" << name << "]: " << spv.error_message.c_str());
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nbuf, 0U);
        REQUIRE(pipe != nullptr);
        crd::containers::Array<float> host_store(&alloc);
        host_store.resize(uz(total), 0.0F);
        float* host[6];
        off = 0;
        for (int i = 0; i < nbuf; ++i)
        {
            host[i] = host_store.data() + off;
            for (int j = 0; j < lens[i]; ++j) { host[i][j] = static_cast<float>(data[i][j]); }
            off += lens[i];
        }
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, nbuf, groups);
        double worst = 0.0;
        for (int j = 0; j < lens[check]; ++j)
        {
            worst = std::max(worst, std::fabs(static_cast<double>(host[check][j]) - ref[uz(j)]));
        }
        std::printf("[Vulkan B18-c %s] maxabs(GPU vs oracle) = %.3e\n", name, worst);
        return worst;
    };

    // ── (1) the shared scattering-moment LUT — the heaviest kernel here (a full BCSDF evaluation inside the integration loop)
    //        and the one whose post-loop RMW normalisation is where the inline-load read-after-write hazard lived. ──
    {
        hms::HairScatterLutConfig lc;
        lc.n_theta_d = 64; lc.n_h = 2; lc.n_theta_o = 8; lc.n_phi_o = 16;
        kir::KGraph       g(&alloc);
        const kir::KEntry e = hms::build_hair_scatter_lut_kernel(g, lc);
        crd::containers::Array<double> out(&alloc);
        out.resize(uz(64 * hms::kLutStride), 0.0);
        double*   data[1] = {out.data()};
        const int lens[1] = {64 * hms::kLutStride};
        CHECK(both(g, e, data, lens, 1, 0, 1U, "scatter_lut") < 3.0e-3); // conformance envelope (llvmpipe 8e-4; NV 1e-6) — the exp-amplified cone again
    }

    // ── (2) volumetric multiple scattering (Hu 2026) ──
    {
        hms::VolumeMsConfig vc;
        kir::KGraph         g(&alloc);
        const kir::KEntry   e = hms::build_volume_ms_kernel(g, vc);
        crd::containers::Array<double> in(&alloc);
        crd::containers::Array<double> out(&alloc);
        in.resize(uz(64 * 5), 0.0);
        out.resize(uz(64 * 2), 0.0);
        for (int k = 0; k < 64; ++k)
        {
            const crd::usize o = uz(k * 5);
            in[o + 0U] = 2.0;
            in[o + 1U] = static_cast<double>(k) / 64.0;
            in[o + 2U] = 0.8;
            in[o + 3U] = 0.25;
            in[o + 4U] = 1.5;
        }
        double*   data[2] = {in.data(), out.data()};
        const int lens[2] = {64 * 5, 64 * 2};
        CHECK(both(g, e, data, lens, 2, 1, 1U, "volume_ms") < 1.0e-5);
    }

    // ── (3) deep opacity map lookup (the build pass feeds it) ──
    {
        hms::DomConfig dc;
        dc.layers = 4; dc.span = 4.0; dc.frags_per_px = 16;
        const int stride = 1 + dc.layers;
        crd::containers::Array<double> frags(&alloc);
        crd::containers::Array<double> dom(&alloc);
        frags.resize(uz(64 * 16 * 2), 0.0);
        dom.resize(uz(64 * stride), 0.0);
        for (int p = 0; p < 64; ++p)
        {
            const double z0 = 1.0 + 0.05 * static_cast<double>(p);
            for (int f = 0; f < 16; ++f)
            {
                const crd::usize o = uz((p * 16 + f) * 2);
                frags[o + 0U] = z0 + 3.0 * ((static_cast<double>(f) + 0.5) / 16.0);
                frags[o + 1U] = 0.1;
            }
        }
        {
            kir::KGraph       g(&alloc);
            const kir::KEntry e = hms::build_dom_build_kernel(g, dc);
            double*   data[2] = {frags.data(), dom.data()};
            const int lens[2] = {64 * 16 * 2, 64 * stride};
            CHECK(both(g, e, data, lens, 2, 1, 1U, "dom_build") < 1.0e-5);
        }
        // rebuild the DOM on the CPU so the lookup has valid input, then gate the lookup itself
        {
            kir::KGraph       gb(&alloc);
            const kir::KEntry eb = hms::build_dom_build_kernel(gb, dc);
            kir::KernelBuffer bb[2] = {{frags.data(), 64 * 16 * 2, 0, 0}, {dom.data(), 64 * stride, 0, 1}};
            kir::eval_cpu_kernel(gb, eb, bb, 2, eb.local_size[0], &alloc, 1U);
        }
        kir::KGraph       g(&alloc);
        const kir::KEntry e = hms::build_dom_lookup_kernel(g, dc);
        crd::containers::Array<double> qry(&alloc);
        crd::containers::Array<double> out(&alloc);
        qry.resize(uz(64 * 2), 0.0);
        out.resize(uz(64 * 2), 0.0);
        for (int p = 0; p < 64; ++p)
        {
            qry[uz(p * 2) + 0U] = static_cast<double>(p);
            qry[uz(p * 2) + 1U] = 1.0 + 0.05 * static_cast<double>(p) + 1.7;
        }
        double*   data[3] = {dom.data(), qry.data(), out.data()};
        const int lens[3] = {64 * stride, 64 * 2, 64 * 2};
        CHECK(both(g, e, data, lens, 3, 2, 1U, "dom_lookup") < 1.0e-5);
    }
}

// B18-f: the LINEAR SWEPT SPHERE strand intersector DISPATCHES on Vulkan == CPU oracle.
// ⚠ This machine is Ada (RTX 4070 Ti SUPER) and does NOT advertise VK_NV_ray_tracing_linear_swept_spheres — that is
//   Blackwell silicon. So this analytic round-cone intersector is not a fallback, it is THE strand primitive here, and it
//   has to be gated as first-class code on both backends.
TEST_CASE("B18-f: CKIR linear-swept-sphere intersector DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][hair][lss]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };
    crd::memory::TlsfAllocator alloc(64U << 20U);

    const int nray = 128;
    const int nseg = 8;
    kir::lss::LssTraceConfig lc;
    lc.segments = nseg;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::lss::build_lss_trace_kernel(g, lc);

    crd::containers::Array<double> rays(&alloc);
    crd::containers::Array<double> segs(&alloc);
    crd::containers::Array<double> out(&alloc);
    rays.resize(uz(nray * 6), 0.0);
    segs.resize(uz(nseg * 8), 0.0);
    out.resize(uz(nray * 2), 0.0);
    crd::u32   st  = 0x1551F00DU;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int s = 0; s < nseg; ++s)
    {
        const crd::usize o = uz(s * 8);
        for (int k = 0; k < 3; ++k) { segs[o + uz(k)] = rnd() * 2.0 - 1.0; }
        segs[o + 3U] = 0.05 + rnd() * 0.25;
        for (int k = 0; k < 3; ++k) { segs[o + 4U + uz(k)] = rnd() * 2.0 - 1.0; }
        segs[o + 7U] = 0.05 + rnd() * 0.25;
    }
    for (int r = 0; r < nray; ++r)
    {
        const crd::usize o = uz(r * 6);
        rays[o + 0U] = rnd() * 4.0 - 2.0;
        rays[o + 1U] = -3.0;
        rays[o + 2U] = rnd() * 4.0 - 2.0;
        rays[o + 3U] = rnd() * 0.4 - 0.2;
        rays[o + 4U] = 1.0;
        rays[o + 5U] = rnd() * 0.4 - 0.2;
    }
    crd::containers::Array<double> snap(&alloc);
    snap.resize(uz(nray * 6 + nseg * 8), 0.0);
    for (int j = 0; j < nray * 6; ++j) { snap[uz(j)] = rays[uz(j)]; }
    for (int j = 0; j < nseg * 8; ++j) { snap[uz(nray * 6 + j)] = segs[uz(j)]; }

    kir::KernelBuffer bufs[3] = {{rays.data(), nray * 6, 0U, 0U},
                                 {segs.data(), nseg * 8, 0U, 1U},
                                 {out.data(), nray * 2, 0U, 2U}};
    kir::eval_cpu_kernel(g, e, bufs, 3, e.local_size[0], &alloc, static_cast<crd::u32>(nray / 64));
    crd::containers::Array<double> ref(&alloc);
    ref.resize(uz(nray * 2), 0.0);
    for (int j = 0; j < nray * 2; ++j) { ref[uz(j)] = out[uz(j)]; }

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_lss_trace", &alloc, false);
    INFO("GLSL compile: " << spv.error_message.c_str());
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U);
    REQUIRE(pipe != nullptr);
    crd::containers::Array<float> host_store(&alloc);
    host_store.resize(uz(nray * 6 + nseg * 8 + nray * 2), 0.0F);
    float*    host[3] = {host_store.data(), host_store.data() + nray * 6, host_store.data() + nray * 6 + nseg * 8};
    const int lens[3] = {nray * 6, nseg * 8, nray * 2};
    for (int j = 0; j < nray * 6; ++j) { host[0][j] = static_cast<float>(snap[uz(j)]); }
    for (int j = 0; j < nseg * 8; ++j) { host[1][j] = static_cast<float>(snap[uz(nray * 6 + j)]); }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 3, static_cast<crd::u32>(nray / 64));

    // Compare only where the CPU reference actually HIT. A miss is sentinel-valued (1e30), and comparing sentinels in
    // F32 is meaningless — what matters is that both agree on WHICH rays hit, and on the distance where they did.
    int    hits = 0;
    int    disagree = 0;
    double worst = 0.0;
    for (int r = 0; r < nray; ++r)
    {
        const bool cpu_hit = ref[uz(r * 2)] < 1.0e29;
        const bool gpu_hit = static_cast<double>(host[2][r * 2]) < 1.0e29;
        if (cpu_hit != gpu_hit) { ++disagree; continue; }
        if (!cpu_hit) { continue; }
        ++hits;
        worst = std::max(worst, std::fabs(static_cast<double>(host[2][r * 2]) - ref[uz(r * 2)]));
        worst = std::max(worst, std::fabs(static_cast<double>(host[2][r * 2 + 1]) - ref[uz(r * 2 + 1)]));
    }
    std::printf("[Vulkan B18-f LSS] %d rays, %d hits  maxabs(GPU vs oracle) = %.3e  hit/miss disagreements = %d\n",
                nray, hits, worst, disagree);
    CHECK(hits > 16);      // the scene must actually be hit, or the comparison proves nothing
    CHECK(disagree == 0);  // both must agree on WHICH rays hit — a boundary disagreement is a hole in the groom
    CHECK(worst < 1.0e-4);
}

// B18-d: the strand LOD pre-pass (Lipp 2026 Eq 1-5, 8) DISPATCHES on Vulkan == CPU oracle. The maths is gated on the CPU
// side (tests/kir/test_ckir_hair_lod.cpp); THIS gate is portability plus the integer-ish ops the emitters must get right —
// Ceil, Floor, Log2 and Exp2 all appear here, and the power-of-two-plus-one snap in Eq 5 is exactly the kind of thing a
// backend can round differently. A one-ULP disagreement there changes the control-point count and thus the geometry.
TEST_CASE("B18-d: CKIR strand LOD pre-pass DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][hair][lod]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };
    crd::memory::TlsfAllocator alloc(32U << 20U);

    kir::hairgeom::StrandLodConfig lc;
    lc.strands_per_bundle = 128;
    const int nb          = 128; // 2 workgroups

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairgeom::build_strand_lod_kernel(g, lc);

    crd::containers::Array<double> in(&alloc);
    crd::containers::Array<double> out(&alloc);
    in.resize(uz(nb * 5), 0.0);
    out.resize(uz(nb * 4), 0.0);
    crd::u32   st  = 0xB18D0000U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int b = 0; b < nb; ++b)
    {
        const crd::usize o = uz(b * 5);
        in[o + 0U] = 0.0;
        in[o + 1U] = 0.0;
        in[o + 2U] = rnd() * 200.0; // screen AABB extent, spanning far-field through full-detail
        in[o + 3U] = rnd() * 200.0;
        in[o + 4U] = rnd();         // per-bundle delta ~ U[0,1)
    }
    crd::containers::Array<double> snap(&alloc);
    snap.resize(uz(nb * 5), 0.0);
    for (int j = 0; j < nb * 5; ++j) { snap[uz(j)] = in[uz(j)]; }

    kir::KernelBuffer bufs[2] = {{in.data(), nb * 5, 0U, 0U}, {out.data(), nb * 4, 0U, 1U}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(nb / 64));
    crd::containers::Array<double> ref(&alloc);
    ref.resize(uz(nb * 4), 0.0);
    for (int j = 0; j < nb * 4; ++j) { ref[uz(j)] = out[uz(j)]; }

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_hair_lod", &alloc, false);
    INFO("GLSL compile: " << spv.error_message.c_str());
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(
        crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> host_store(&alloc);
    host_store.resize(uz(nb * 9), 0.0F);
    float*    host[2] = {host_store.data(), host_store.data() + nb * 5};
    const int lens[2] = {nb * 5, nb * 4};
    for (int j = 0; j < nb * 5; ++j) { host[0][j] = static_cast<float>(snap[uz(j)]); }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(nb / 64));

    double worst = 0.0;
    int    cp_mismatch = 0;
    for (int b = 0; b < nb; ++b)
    {
        for (int k = 0; k < 4; ++k)
        {
            const double d = static_cast<double>(host[1][b * 4 + k]) - ref[uz(b * 4 + k)];
            worst = std::max(worst, std::fabs(d));
        }
        // ⭐ N_LOD and the control-point count are DISCRETE. They must agree EXACTLY, not to a tolerance — a half-strand
        //   disagreement is meaningless, but an off-by-one changes how much geometry the backend actually draws.
        if (static_cast<double>(host[1][b * 4 + 0]) != ref[uz(b * 4 + 0)]) { ++cp_mismatch; }
        if (static_cast<double>(host[1][b * 4 + 1]) != ref[uz(b * 4 + 1)]) { ++cp_mismatch; }
    }
    std::printf("[Vulkan B18-d LOD] %d bundles  maxabs(GPU vs oracle) = %.3e  discrete mismatches = %d\n", nb, worst,
                cp_mismatch);
    CHECK(worst < 1.0e-5);
    CHECK(cp_mismatch == 0);
}

// B18-e: the COMPOSITING filter (Lipp 2026) DISPATCHES on Vulkan and matches the CPU oracle. The filter's maths is gated on
// the CPU side (tests/kir/test_ckir_hair_filter.cpp pins anisotropy, the depth guard, and convexity); THIS gate is portability
// plus one thing the CPU tests cannot show: the kernel's BOUNDS GUARD. A real dispatch rounds up to workgroup granularity, so
// the tail lanes here run past the pixel count on actual hardware — exactly the case that silently corrupts the last pixel if
// the guard is missing.
TEST_CASE("B18-e: CKIR hair compositing filter DISPATCHES on Vulkan == CPU oracle",
          "[gpu-context][vulkan][gpu][kernel][hair][filter]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };
    crd::memory::TlsfAllocator alloc(64U << 20U);

    // 10x9 = 90 pixels over 2 workgroups of 64 ⇒ 38 tail lanes run out of range. That is deliberate.
    kir::hairgeom::HairFilterConfig fc;
    fc.width  = 10;
    fc.height = 9;
    const int np = fc.width * fc.height;

    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hairgeom::build_hair_filter_kernel(g, fc);

    crd::containers::Array<double> col(&alloc);
    crd::containers::Array<double> tan(&alloc);
    crd::containers::Array<double> dep(&alloc);
    crd::containers::Array<double> out(&alloc);
    col.resize(uz(np * 3), 0.0);
    tan.resize(uz(np * 2), 0.0);
    dep.resize(uz(np), 0.0);
    out.resize(uz(np * 4), 0.0);
    crd::u32   st  = 0x9E3779B9U;
    const auto rnd = [&]() { st = st * 1664525U + 1013904223U; return static_cast<double>(st >> 8U) / 16777216.0; };
    for (int i = 0; i < np; ++i)
    {
        for (int c = 0; c < 3; ++c) { col[uz(i * 3 + c)] = rnd(); }
        const double a  = rnd() * 6.2831853;
        tan[uz(i * 2 + 0)] = std::cos(a); // unit tangents, varying per pixel — exercises the ellipse rotation on device
        tan[uz(i * 2 + 1)] = std::sin(a);
        dep[uz(i)]         = 0.5 + 0.002 * rnd(); // tight spread ⇒ the depth guard both passes and rejects
    }

    crd::containers::Array<double> snap(&alloc);
    snap.resize(uz(np * 3 + np * 2 + np), 0.0);
    for (int j = 0; j < np * 3; ++j) { snap[uz(j)] = col[uz(j)]; }
    for (int j = 0; j < np * 2; ++j) { snap[uz(np * 3 + j)] = tan[uz(j)]; }
    for (int j = 0; j < np; ++j) { snap[uz(np * 5 + j)] = dep[uz(j)]; }

    const crd::u32    groups  = (static_cast<crd::u32>(np) + e.local_size[0] - 1U) / e.local_size[0];
    kir::KernelBuffer bufs[4] = {{col.data(), np * 3, 0U, 0U},
                                 {tan.data(), np * 2, 0U, 1U},
                                 {dep.data(), np, 0U, 2U},
                                 {out.data(), np * 4, 0U, 3U}};
    kir::eval_cpu_kernel(g, e, bufs, 4, e.local_size[0], &alloc, groups);
    crd::containers::Array<double> ref(&alloc);
    ref.resize(uz(np * 4), 0.0);
    for (int j = 0; j < np * 4; ++j) { ref[uz(j)] = out[uz(j)]; }

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source),
                                                "ckir_hair_filter", &alloc, false);
    INFO("GLSL compile: " << spv.error_message.c_str());
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 4U, 0U);
    REQUIRE(pipe != nullptr);

    crd::containers::Array<float> host_store(&alloc);
    host_store.resize(uz(np * 3 + np * 2 + np + np * 4), 0.0F);
    float*    host[4] = {host_store.data(), host_store.data() + np * 3, host_store.data() + np * 5, host_store.data() + np * 6};
    const int lens[4] = {np * 3, np * 2, np, np * 4};
    for (int j = 0; j < np * 3; ++j) { host[0][j] = static_cast<float>(snap[uz(j)]); }
    for (int j = 0; j < np * 2; ++j) { host[1][j] = static_cast<float>(snap[uz(np * 3 + j)]); }
    for (int j = 0; j < np; ++j) { host[2][j] = static_cast<float>(snap[uz(np * 5 + j)]); }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 4, groups);

    double worst = 0.0;
    for (int j = 0; j < np * 4; ++j) { worst = std::max(worst, std::fabs(static_cast<double>(host[3][j]) - ref[uz(j)])); }
    std::printf("[Vulkan B18-e filter] %dx%d over %u groups (%d tail lanes)  maxabs(GPU vs oracle) = %.3e\n",
                fc.width, fc.height, groups, static_cast<int>(groups * e.local_size[0]) - np, worst);
    CHECK(worst < 1.0e-5);

    // The guard held: every pixel still carries a strictly positive weight sum (it gathers itself at minimum). A tail lane
    // that escaped the guard would have RMW-accumulated into a clamped address and knocked one of these off its oracle value.
    for (int i = 0; i < np; ++i) { CHECK(host[3][i * 4 + 3] > 0.0F); }
}


// B18-b: the HUANG 2022 microfacet R lobe (analytic GGX azimuthal integral, Appendix A) DISPATCHES on Vulkan == CPU oracle to-ULP.
// The CPU side already proves the closed form equals a 20k-sample quadrature (tests/kir); this proves the SAME graph lowers and
// runs identically on the device — atan2/tan-free stable branch included.
TEST_CASE("B18-b: CKIR Huang microfacet R lobe DISPATCHES on Vulkan == CPU oracle (to-ULP)",
          "[gpu-context][vulkan][gpu][kernel][hair][huang]")
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
    const auto   uz  = [](int v) { return static_cast<crd::usize>(v); };
    const double kpi = kir::hair::kPi;

    crd::memory::TlsfAllocator  alloc(64U << 20U);
    kir::hair::HairKernelConfig hcfg;
    // HuangFull = analytic R + the TT/TRT combined MC-Simpson estimator (include_r defaults true), so this one dispatch gates
    // BOTH halves: the closed-form azimuthal integral AND the runtime Simpson loop with its VNDF/refraction chain and triple32
    // uniforms. A short Simpson count keeps the GPU gate quick; the count's correctness is established on the CPU side.
    hcfg.model      = kir::hair::HairModel::HuangFull;
    hcfg.huang_beta = 0.3;
    hcfg.simpson_n  = 12;
    const int         n = 256;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = kir::hair::build_hair_bcsdf_kernel(g, hcfg);

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n * 6));
    out.resize(uz(n));
    crd::u32 s   = 91177U;
    auto     rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int i = 0; i < n; ++i)
    {
        in[uz(i * 6 + 0)] = (rnd() * 2.0 - 1.0) * 0.93; // sinθo
        in[uz(i * 6 + 1)] = (rnd() * 2.0 - 1.0) * kpi;  // φo
        in[uz(i * 6 + 2)] = (rnd() * 2.0 - 1.0) * 0.93; // sinθi
        in[uz(i * 6 + 3)] = (rnd() * 2.0 - 1.0) * kpi;  // φi
        in[uz(i * 6 + 4)] = rnd() * 2.0 - 1.0;          // h  (unused by the Huang R lobe)
        in[uz(i * 6 + 5)] = rnd() * 1.5;                // σₐ (unused by the Huang R lobe)
    }
    kir::KernelBuffer bufs[2] = {{in.data(), n * 6, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, static_cast<crd::u32>(n / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    std::printf("[Huang GLSL] emitted source = %zu chars\n", static_cast<size_t>(kern.source.size()));
    std::fflush(stdout);
    // ⚠ optimize = FALSE: the HuangFull kernel is a big graph (two VNDF samplers + the full refraction chain inside a Simpson
    // For loop). spirv-opt does NOT terminate in reasonable time on it — unlike the compact cloud kernel. Unoptimized SPIR-V is
    // fine here: this gate measures NUMERIC agreement with the oracle, not shader performance.
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_huang", &alloc, false);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {n * 6, n};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(n * 6));
    h1.resize(uz(n));
    for (int i = 0; i < n * 6; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    float* host[2] = {h0.data(), h1.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(n / 64));

    double maxabs = 0.0;
    double maxrel = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double gv = static_cast<double>(h1[uz(i)]);
        const double ov = out[uz(i)];
        const double ad = std::fabs(gv - ov);
        maxabs          = std::max(maxabs, ad);
        if (std::fabs(ov) > 1.0e-3) { maxrel = std::max(maxrel, ad / std::fabs(ov)); }
    }
    std::printf("[Vulkan Huang full] maxabs(GPU vs oracle) = %.3e  maxrel = %.3e\n", maxabs, maxrel);
    // TOLERANCE RATIONALE (deliberately looser than the hair/fur gates' 3e-5, and justified — not moved to fit):
    // those evaluate ONE shallow cone (~50 transcendental ops). HuangFull is ~20x that — 13 Simpson nodes, each with two VNDF
    // samplers, a refraction, four Fresnel evaluations and exp/atan2/normalize chains — PLUS a 13-term f32 accumulation, so
    // rounding compounds. maxabs stays TIGHT at the same 1e-5 (absolute agreement is excellent); only the relative metric on
    // near-zero lanes drifts. Observed maxrel 1.03e-4; bound 5e-4 keeps ~5x cross-driver margin and matches the codebase's
    // precedent for a transcendental-heavy kernel (the SVGF gate uses 1e-4).
    CHECK(maxabs < 1.0e-5);
    CHECK(maxrel < 5.0e-4);
}

TEST_CASE("B15-b: CKIR cloud RAY-MARCH (Beer-Powder + phase + light march over the density volume) DISPATCHES on Vulkan == oracle",
          "[gpu-context][vulkan][gpu][kernel][clouds]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::clouds::CloudConfig   ccfg;
    const int                  dim = ccfg.vol_dim;
    const int                  sw  = ccfg.screen;
    const int                  nv  = dim * dim * dim;
    const int                  ns  = sw * sw;
    const double               ext = 6.0;
    const double               scl = ext / static_cast<double>(dim - 1);
    const double               hsc = (ccfg.cloud_top - ccfg.cloud_base) / static_cast<double>(dim - 1);

    // bake the density volume on the CPU oracle, f32-round it (so the oracle + GPU march sample IDENTICAL input).
    crd::containers::Array<crd::f64> pos(&alloc);
    pos.resize(uz(nv * 3));
    for (int zi = 0; zi < dim; ++zi) { for (int yi = 0; yi < dim; ++yi) { for (int xi = 0; xi < dim; ++xi) {
        const int cell = (zi * dim + yi) * dim + xi;
        pos[uz(cell * 3 + 0)] = static_cast<double>(xi) * scl;
        pos[uz(cell * 3 + 1)] = static_cast<double>(yi) * scl;
        pos[uz(cell * 3 + 2)] = ccfg.cloud_base + static_cast<double>(zi) * hsc;
    } } }
    kir::KGraph       gd(&alloc);
    const kir::KEntry ed = kir::clouds::build_cloud_density(gd, ccfg);
    crd::containers::Array<crd::f64> vol(&alloc);
    vol.resize(uz(nv));
    kir::KernelBuffer db[2] = {{pos.data(), nv * 3, 0, 0}, {vol.data(), nv, 0, 1}};
    kir::eval_cpu_kernel(gd, ed, db, 2, ed.local_size[0], &alloc, static_cast<crd::u32>(nv / 64));
    for (int i = 0; i < nv; ++i) { vol[uz(i)] = static_cast<double>(static_cast<float>(vol[uz(i)])); }

    kir::KGraph       gm(&alloc);
    const kir::KEntry em = kir::clouds::build_cloud_march(gm, ccfg);
    crd::containers::Array<crd::f64> out(&alloc);
    out.resize(uz(ns * 4));
    kir::KernelBuffer mb[2] = {{vol.data(), nv, 0, 0}, {out.data(), ns * 4, 0, 1}};
    kir::eval_cpu_kernel(gm, em, mb, 2, em.local_size[0], &alloc, static_cast<crd::u32>(ns / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(gm, em, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_cloud_march", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[2] = {nv, ns * 4};
    crd::containers::Array<float> h0(&alloc);
    crd::containers::Array<float> h1(&alloc);
    h0.resize(uz(nv));
    h1.resize(uz(ns * 4));
    for (int i = 0; i < nv; ++i) { h0[uz(i)] = static_cast<float>(vol[uz(i)]); }
    for (int i = 0; i < ns * 4; ++i) { h1[uz(i)] = -9.0F; }
    float* host[2] = {h0.data(), h1.data()};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, static_cast<crd::u32>(ns / 64));

    double maxabs = 0.0;
    for (int i = 0; i < ns * 4; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h1[uz(i)]) - out[uz(i)])); }
    std::printf("[Vulkan cloud march] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    CHECK(maxabs < 5e-5); // trilinear taps + Beer-Powder + phase; only exp/pow are ULP ⇒ effectively bit-exact
}

// B16-a-0: the transcendentals the ocean spectrum needs (log/log2/tanh/atan2/atan/asin/acos/sinh/cosh) were only in the raster
// emitter; now wired into the compute-kernel emitters (all 5). This DISPATCHES a compute kernel using all nine on Vulkan and
// verifies GPU == CPU oracle to ULP (transcendentals are hardware-vs-libm ULP, not bit-exact — like exp/pow elsewhere).
TEST_CASE("B16-a-0: compute transcendentals DISPATCH on Vulkan == CPU oracle (ULP)", "[gpu-context][vulkan][gpu][kernel][ocean]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const int                  n     = 64;
    const kir::Shape           sh1   = kir::make_shape({1});
    const int                  inbuf = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int                  outb  = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int                  mark  = g.kernel_stmt_mark();
    const auto                 kf    = [&](double v) { return g.constant(v, sh1, kir::DType::F32); };
    const auto                 un    = [&](kir::KOp op, int a) { return g.unary(op, a); };
    const auto                 bi    = [&](kir::KOp op, int a, int b) { return g.binary(op, a, b); };
    const int                  gid   = g.builtin(kir::KBuiltin::LocalInvocationIndex); // one workgroup of 64
    const int                  x     = g.buffer_load(inbuf, gid);
    const int                  xh    = bi(kir::KOp::Mul, x, kf(0.5));
    int                        acc   = un(kir::KOp::Log, bi(kir::KOp::Add, x, kf(1.0)));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Log2, bi(kir::KOp::Add, x, kf(2.0))));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Tanh, x));
    acc                              = bi(kir::KOp::Add, acc, bi(kir::KOp::Atan2, x, kf(0.5)));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Atan, x));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Asin, xh));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Acos, xh));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Sinh, x));
    acc                              = bi(kir::KOp::Add, acc, un(kir::KOp::Cosh, x));
    g.stmt_buffer_store(outb, gid, acc);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = static_cast<crd::u32>(n);
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    crd::containers::Array<crd::f64> in(&alloc);
    crd::containers::Array<crd::f64> out(&alloc);
    in.resize(uz(n));
    out.resize(uz(n));
    for (int i = 0; i < n; ++i) { in[uz(i)] = 0.2 + 0.01 * static_cast<double>(i); }
    kir::KernelBuffer bufs[2] = {{in.data(), n, 0, 0}, {out.data(), n, 0, 1}};
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, 1U);

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_transc", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 2, 0U);
    REQUIRE(pipe != nullptr);

    const int                     lens[2] = {n, n};
    crd::containers::Array<float>  h0(&alloc);
    crd::containers::Array<float>  h1(&alloc);
    float*                        host[2] = {nullptr, nullptr};
    h0.resize(uz(n));
    h1.resize(uz(n));
    host[0] = h0.data();
    host[1] = h1.data();
    for (int i = 0; i < n; ++i) { h0[uz(i)] = static_cast<float>(in[uz(i)]); }
    for (int i = 0; i < n; ++i) { h1[uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);

    double maxabs = 0.0;
    for (int i = 0; i < n; ++i) { maxabs = std::max(maxabs, std::fabs(static_cast<double>(h1[uz(i)]) - out[uz(i)])); }
    std::printf("[Vulkan transcendentals] maxabs(GPU vs oracle) = %.2e\n", maxabs);
    // conformance envelope: the Vulkan spec grants atan2/asin/acos 4096 ULP (~5e-4 relative) and the 9-op sum
    // compounds to ~1e-3 absolute at these magnitudes. llvmpipe measures 1.6e-4; NV 1e-6. Both conformant.
    CHECK(maxabs < 1e-3); // log/atan2/asin/… are hardware-vs-libm ULP; the whole 9-op sum stays within a few ULP
}

// The B14 GI + B15 atmosphere kernels' GPU THROUGHPUT on Vulkan — GPU-only time via `last_gpu_ms` (kernel only, upload
// excluded), min-of-30. These same kernels are verified BIT-EXACT vs the CPU oracle in the tests above; this proves the
// PORTABLE IR also runs in real time. Hidden ([.gi-bench]) — run explicitly. Board → docs/bench/2026-07-15-gi-atmosphere-vulkan.md.
TEST_CASE("B14/B15: CKIR GI + atmosphere kernels GPU PERFORMANCE (Vulkan, last_gpu_ms, min-of-30)", "[.gi-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(256U << 20U);

    const auto mkbuf = [&](crd::u64 floats) { return compute.create_buffer(floats * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); };
    const auto fillbuf = [&](cg::ComputeBuffer& dev, crd::u64 floats, float v) {
        auto  stg = compute.create_buffer(floats * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* pp  = static_cast<float*>(stg->map());
        for (crd::u64 i = 0; i < floats; ++i) { pp[i] = v; }
        stg->unmap();
        auto& rec = compute.begin();
        rec.copy(*stg, dev, 0U, 0U, floats * sizeof(float));
        compute.submit_and_wait();
    };
    const auto pipe_of = [&](kir::KGraph& g, const kir::KEntry& e, const char* nm, int nb) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), nm, &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
        REQUIRE(pipe != nullptr);
        return pipe;
    };
    const auto time_best = [&](cg::ComputePipeline& pipe, cg::ComputeBuffer** binds, int nb, crd::u32 gx) {
        double best = 1e30;
        for (int r = 0; r < 30; ++r)
        {
            auto& rec = compute.begin();
            rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(nb)), nullptr, 0U, gx, 1U, 1U);
            compute.submit_and_wait();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < best) { best = ms; }
        }
        return best;
    };

    std::printf("\n=== CKIR GI + atmosphere GPU throughput (Vulkan, kernel-only) ===\n");

    { // B14-b DDGI probe SAMPLE — the shading-time indirect-diffuse lookup, one thread per query.
        const int nq = 1920 * 1080; // one probe-sample per 1080p pixel
        const int r  = 8;
        kir::KGraph           g(&alloc);
        kir::ddgi::DdgiConfig dc;
        const kir::KEntry     e    = kir::ddgi::build_ddgi_sample(g, dc);
        auto                  pipe = pipe_of(g, e, "bench_ddgi_sample", 5);
        auto pos = mkbuf(static_cast<crd::u64>(nq) * 3);
        auto nrm = mkbuf(static_cast<crd::u64>(nq) * 3);
        const crd::u64 irr_n = static_cast<crd::u64>(r) * static_cast<crd::u64>(r) * 24U; // 8 probes · r² · 3
        const crd::u64 dpt_n = static_cast<crd::u64>(r) * static_cast<crd::u64>(r) * 16U; // 8 probes · r² · 2
        auto irr = mkbuf(irr_n);
        auto dpt = mkbuf(dpt_n);
        auto out = mkbuf(static_cast<crd::u64>(nq) * 3);
        fillbuf(*pos, static_cast<crd::u64>(nq) * 3, 0.5F);
        fillbuf(*nrm, static_cast<crd::u64>(nq) * 3, 0.5F);
        fillbuf(*irr, irr_n, 0.4F);
        fillbuf(*dpt, dpt_n, 4.0F);
        cg::ComputeBuffer* b[5] = {pos.get(), nrm.get(), irr.get(), dpt.get(), out.get()};
        const double       ms   = time_best(*pipe, b, 5, static_cast<crd::u32>(nq / 64));
        std::printf("[gi-bench] DDGI probe-sample      %9d queries  %7.3f ms  %8.1f Mqueries/s\n", nq, ms, static_cast<double>(nq) / ms / 1e3);
    }

    { // B14-a ReSTIR RIS — stream M candidates through each pixel's reservoir.
        const int n = 512 * 512; // 262k pixels (the M·N candidate buffer scales with M)
        kir::KGraph               g(&alloc);
        kir::restir::RestirConfig rc;
        const int                 m    = rc.num_candidates;
        const kir::KEntry         e    = kir::restir::build_restir_ris(g, rc);
        auto                      pipe = pipe_of(g, e, "bench_restir_ris", 2);
        auto cand = mkbuf(static_cast<crd::u64>(n) * m * 3);
        auto res  = mkbuf(static_cast<crd::u64>(n) * 4);
        fillbuf(*cand, static_cast<crd::u64>(n) * m * 3, 0.5F);
        cg::ComputeBuffer* b[2] = {cand.get(), res.get()};
        const double       ms   = time_best(*pipe, b, 2, static_cast<crd::u32>(n / 64));
        std::printf("[gi-bench] ReSTIR RIS (M=%2d)       %9d pixels   %7.3f ms  %8.1f Mpixels/s\n", m, n, ms, static_cast<double>(n) / ms / 1e3);
    }

    { // B14-c SVGF à-trous — one edge-stopping wavelet iteration, one thread per pixel.
        const int         w = 1920;
        const int         h = 1080;
        const int         n = w * h;
        kir::KGraph       g(&alloc);
        kir::SvgfConfig   sc;
        sc.width  = w;
        sc.height = h;
        sc.step   = 1;
        const kir::KEntry e    = kir::build_svgf_atrous(g, sc);
        auto              pipe = pipe_of(g, e, "bench_svgf_atrous", 5);
        auto col = mkbuf(static_cast<crd::u64>(n) * 3);
        auto gbf = mkbuf(static_cast<crd::u64>(n) * 4);
        auto vin = mkbuf(static_cast<crd::u64>(n));
        auto cot = mkbuf(static_cast<crd::u64>(n) * 3);
        auto vot = mkbuf(static_cast<crd::u64>(n));
        fillbuf(*col, static_cast<crd::u64>(n) * 3, 0.5F);
        fillbuf(*gbf, static_cast<crd::u64>(n) * 4, 1.0F);
        fillbuf(*vin, static_cast<crd::u64>(n), 0.1F);
        cg::ComputeBuffer* b[5] = {col.get(), gbf.get(), vin.get(), cot.get(), vot.get()};
        const double       ms   = time_best(*pipe, b, 5, static_cast<crd::u32>(n / 64));
        std::printf("[gi-bench] SVGF a-trous (1 iter)  %9d pixels   %7.3f ms  %8.1f Mpixels/s\n", n, ms, static_cast<double>(n) / ms / 1e3);
    }

    { // B15-a atmosphere: the full sky LUT chain (transmittance + multiscatter + sky-view + aerial) — cost PER FRAME.
        kir::atmos::AtmosphereConfig ac;
        const auto bench_lut = [&](const char* nm, kir::KEntry (*build)(kir::KGraph&, const kir::atmos::AtmosphereConfig&), int nb,
                                   const int* fsz, crd::u32 gx, int work, const char* unit) {
            kir::KGraph g(&alloc);
            const kir::KEntry e = build(g, ac);
            auto pipe = pipe_of(g, e, nm, nb);
            crd::containers::Array<std::unique_ptr<cg::ComputeBuffer>> bufs(&alloc);
            for (int i = 0; i < nb; ++i) { auto bp = mkbuf(static_cast<crd::u64>(fsz[i])); fillbuf(*bp, static_cast<crd::u64>(fsz[i]), 0.5F); bufs.push_back(std::move(bp)); }
            cg::ComputeBuffer* b[4] = {nullptr, nullptr, nullptr, nullptr};
            for (int i = 0; i < nb; ++i) { b[i] = bufs[static_cast<crd::usize>(i)].get(); }
            const double ms = time_best(*pipe, b, nb, gx);
            std::printf("[gi-bench] atmos %-16s %9d %-8s %7.4f ms\n", nm, work, unit, ms);
            (void)unit;
        };
        const int tw = ac.tlut_w;
        const int th = ac.tlut_h;
        const int mr = ac.mslut_res;
        const int sw = ac.skyview_w;
        const int sh = ac.skyview_h;
        const int t_sz[1] = {tw * th * 3};
        bench_lut("transmittance", &kir::atmos::build_atmos_transmittance, 1, t_sz, static_cast<crd::u32>(tw * th / 64), tw * th, "texels");
        const int m_sz[2] = {tw * th * 3, mr * mr * 3};
        bench_lut("multiscatter", &kir::atmos::build_atmos_multiscatter, 2, m_sz, static_cast<crd::u32>(mr * mr / 64), mr * mr, "texels");
        const int s_sz[3] = {tw * th * 3, mr * mr * 3, sw * sh * 3};
        bench_lut("sky-view", &kir::atmos::build_atmos_skyview, 3, s_sz, static_cast<crd::u32>(sw * sh / 64), sw * sh, "texels");
        const int a_sz[3] = {tw * th * 3, mr * mr * 3, ac.ap_res * ac.ap_res * ac.ap_slices * 4};
        bench_lut("aerial-persp", &kir::atmos::build_atmos_aerial, 3, a_sz, static_cast<crd::u32>(ac.ap_res * ac.ap_res / 64), ac.ap_res * ac.ap_res * ac.ap_slices, "cells");
    }
    std::printf("================================================================\n");
}

// THE HONEST CRUSH — CKIR-emitted GLSL vs HAND-WRITTEN GLSL, same algorithm, same buffers, GPU-timed. Proves the portable IR
// carries NO performance overhead: the emitter's SSA-of-`precise`-temps compiles to code the driver optimizes as well as a
// human's hand-written shader. The honesty gate: both kernels' OUTPUTS must match (a fast WRONG hand kernel is meaningless).
// Hidden ([.crush-bench]). Board → docs/bench/2026-07-15-ckir-vs-handwritten-glsl.md.
TEST_CASE("B14/B15: CKIR-emitted GLSL vs HAND-WRITTEN GLSL head-to-head (zero-IR-overhead proof, Vulkan)", "[.crush-bench]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(256U << 20U);

    const auto mkbuf = [&](crd::u64 floats) { return compute.create_buffer(floats * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly); };
    const auto fillbuf = [&](cg::ComputeBuffer& dev, crd::u64 floats, auto gen) {
        auto  stg = compute.create_buffer(floats * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* pp  = static_cast<float*>(stg->map());
        for (crd::u64 i = 0; i < floats; ++i) { pp[i] = gen(i); }
        stg->unmap();
        auto& rec = compute.begin();
        rec.copy(*stg, dev, 0U, 0U, floats * sizeof(float));
        compute.submit_and_wait();
    };
    const auto pipe_src = [&](crd::containers::StringView src, const char* nm, int nb) {
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, src, nm, &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
        REQUIRE(pipe != nullptr);
        return pipe;
    };
    const auto time_best = [&](cg::ComputePipeline& pipe, cg::ComputeBuffer** binds, int nb, crd::u32 gx) {
        double best = 1e30;
        for (int r = 0; r < 30; ++r)
        {
            auto& rec = compute.begin();
            rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(nb)), nullptr, 0U, gx, 1U, 1U);
            compute.submit_and_wait();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < best) { best = ms; }
        }
        return best;
    };
    const auto maxabs_diff = [&](cg::ComputeBuffer& a, cg::ComputeBuffer& b, crd::u64 floats) {
        auto ra = compute.create_buffer(floats * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto rb = compute.create_buffer(floats * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto& rec = compute.begin();
        rec.copy(a, *ra, 0U, 0U, floats * sizeof(float));
        rec.copy(b, *rb, 0U, 0U, floats * sizeof(float));
        compute.submit_and_wait();
        const auto* pa = static_cast<const float*>(ra->map());
        const auto* pb = static_cast<const float*>(rb->map());
        double      md = 0.0;
        for (crd::u64 i = 0; i < floats; ++i) { md = std::max(md, std::fabs(static_cast<double>(pa[i]) - static_cast<double>(pb[i]))); }
        ra->unmap();
        rb->unmap();
        return md;
    };

    std::printf("\n=== CKIR-emitted GLSL vs hand-written GLSL (Vulkan, kernel-only, min-of-30) ===\n");

    { // ReSTIR RIS — the M=32 weighted-reservoir loop (compute-bound).
        const int n = 512 * 512;
        kir::KGraph               g(&alloc);
        kir::restir::RestirConfig rc;
        const int                 m = rc.num_candidates;
        const kir::KEntry         e = kir::restir::build_restir_ris(g, rc);
        kir::GlslKernel           kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        auto pipe_c = pipe_src(crd::containers::to_view(kern.source), "ris_ckir", 2);

        crd::containers::String hand(&alloc);
        hand.append(R"(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer B0 { float buf0[]; };
layout(std430, binding = 1) buffer B1 { float buf1[]; };
void main() {
  uint p = gl_WorkGroupID.x * 64u + gl_LocalInvocationID.x;
  float wsum = 0.0, cf = 0.0, cph = 0.0;
  for (int i = 0; i < 32; ++i) {
    uint base = (p * 32u + uint(i)) * 3u;
    float fi = buf0[base]; float phi = buf0[base + 1u]; float xii = buf0[base + 2u];
    wsum += phi;
    bool repl = xii < phi / max(wsum, 1e-8);
    cf = repl ? fi : cf; cph = repl ? phi : cph;
  }
  float w = wsum / (32.0 * max(cph, 1e-8));
  uint o = p * 4u;
  buf1[o] = cf; buf1[o + 1u] = cph; buf1[o + 2u] = w; buf1[o + 3u] = 32.0;
}
)");
        auto pipe_h = pipe_src(crd::containers::to_view(hand), "ris_hand", 2);

        // a SHARED-accumulator loop variant — what CKIR's stmt_for_begin (the only compute-path runtime loop, carrying state
        // through shared memory) would emit — to see whether a tight shared loop closes the unroll gap or serialises.
        crd::containers::String hsh(&alloc);
        hsh.append(R"(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer B0 { float buf0[]; };
layout(std430, binding = 1) buffer B1 { float buf1[]; };
shared float sw[64]; shared float scf[64]; shared float scph[64];
void main() {
  uint p = gl_WorkGroupID.x * 64u + gl_LocalInvocationID.x;
  uint tid = gl_LocalInvocationIndex;
  sw[tid] = 0.0; scf[tid] = 0.0; scph[tid] = 0.0;
  for (int i = 0; i < 32; ++i) {
    uint base = (p * 32u + uint(i)) * 3u;
    float phi = buf0[base + 1u];
    float wsum = sw[tid] + phi; sw[tid] = wsum;
    bool repl = buf0[base + 2u] < phi / max(wsum, 1e-8);
    scf[tid] = repl ? buf0[base] : scf[tid];
    scph[tid] = repl ? phi : scph[tid];
  }
  float w = sw[tid] / (32.0 * max(scph[tid], 1e-8));
  uint o = p * 4u;
  buf1[o] = scf[tid]; buf1[o + 1u] = scph[tid]; buf1[o + 2u] = w; buf1[o + 3u] = 32.0;
}
)");
        auto pipe_s = pipe_src(crd::containers::to_view(hsh), "ris_shared", 2);

        auto cand  = mkbuf(static_cast<crd::u64>(n) * m * 3);
        auto out_c = mkbuf(static_cast<crd::u64>(n) * 4);
        auto out_h = mkbuf(static_cast<crd::u64>(n) * 4);
        auto out_s = mkbuf(static_cast<crd::u64>(n) * 4);
        fillbuf(*cand, static_cast<crd::u64>(n) * m * 3, [](crd::u64 i) { return 0.2F + 0.3F * static_cast<float>((i * 2654435761ULL >> 13) & 1023ULL) / 1023.0F; });
        cg::ComputeBuffer* bc[2] = {cand.get(), out_c.get()};
        cg::ComputeBuffer* bh[2] = {cand.get(), out_h.get()};
        cg::ComputeBuffer* bs[2] = {cand.get(), out_s.get()};
        const double       msc   = time_best(*pipe_c, bc, 2, static_cast<crd::u32>(n / 64));
        const double       msh   = time_best(*pipe_h, bh, 2, static_cast<crd::u32>(n / 64));
        const double       mss   = time_best(*pipe_s, bs, 2, static_cast<crd::u32>(n / 64));
        const double       md    = maxabs_diff(*out_c, *out_h, static_cast<crd::u64>(n) * 4);
        const double       mds   = maxabs_diff(*out_c, *out_s, static_cast<crd::u64>(n) * 4);
        CHECK(md < 1e-4);  // HONESTY GATE: the hand kernel computes the SAME thing (else the perf number is meaningless)
        CHECK(mds < 1e-4); // the shared-loop variant is also the same reservoir
        std::printf("[crush] ReSTIR RIS (M=32)    CKIR %7.3f ms | hand-reg-loop %7.3f ms | hand-shared-loop %7.3f ms | CKIR/hand %.3f | out-match %.1e/%.1e\n", msc, msh, mss, msc / msh, md, mds);
    }

    { // atmosphere transmittance — the 40-step exp extinction march (compute-bound), a big LUT for a measurable time.
        kir::atmos::AtmosphereConfig ac;
        ac.tlut_w = 2048;
        ac.tlut_h = 512;
        const int         w = ac.tlut_w;
        const int         h = ac.tlut_h;
        const int         n = w * h;
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::atmos::build_atmos_transmittance(g, ac);
        kir::GlslKernel   kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        auto pipe_c = pipe_src(crd::containers::to_view(kern.source), "tlut_ckir", 1);

        crd::containers::String hand(&alloc);
        hand.append(R"(#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) buffer B0 { float buf0[]; };
void main() {
  uint p = gl_WorkGroupID.x * 64u + gl_LocalInvocationID.x;
  const float rg = 6360.0, rt = 6460.0, h2 = 1282000.0, W = 2048.0, H = 512.0;
  float hn = sqrt(h2);
  float fp = float(p);
  float iyf = floor(fp / W); float ixf = fp - iyf * W;
  float uu = (ixf + 0.5) / W; float vv = (iyf + 0.5) / H;
  float v2 = vv * vv;
  float r = sqrt(h2 * v2 + rg * rg);
  float d_min = rt - r; float d_max = hn * (1.0 + vv);
  float d = max(d_min + uu * (d_max - d_min), 1e-4);
  float mu = max(min((h2 * (1.0 - v2) - d * d) / (2.0 * r * d), 1.0), -1.0);
  float dt = d / 40.0; float trm = 2.0 * r * mu;
  float tr = 0.0, tg = 0.0, tb = 0.0;
  for (int i = 0; i < 40; ++i) {
    float t = (float(i) + 0.5) * dt;
    float rtx = sqrt(r * r + trm * t + t * t);
    float alt = max(rtx - rg, 0.0);
    float rr = exp(-alt / 8.0); float rm = exp(-alt / 1.2);
    float ro = max(1.0 - abs(alt - 25.0) / 15.0, 0.0);
    tr += (0.005802 * rr + 0.004440 * rm + 0.000650 * ro) * dt;
    tg += (0.013558 * rr + 0.004440 * rm + 0.001881 * ro) * dt;
    tb += (0.033100 * rr + 0.004440 * rm + 0.000085 * ro) * dt;
  }
  uint o = p * 3u;
  buf0[o] = exp(-tr); buf0[o + 1u] = exp(-tg); buf0[o + 2u] = exp(-tb);
}
)");
        auto pipe_h = pipe_src(crd::containers::to_view(hand), "tlut_hand", 1);

        auto out_c = mkbuf(static_cast<crd::u64>(n) * 3);
        auto out_h = mkbuf(static_cast<crd::u64>(n) * 3);
        cg::ComputeBuffer* bc[1] = {out_c.get()};
        cg::ComputeBuffer* bh[1] = {out_h.get()};
        const double       msc   = time_best(*pipe_c, bc, 1, static_cast<crd::u32>(n / 64));
        const double       msh   = time_best(*pipe_h, bh, 1, static_cast<crd::u32>(n / 64));
        const double       md    = maxabs_diff(*out_c, *out_h, static_cast<crd::u64>(n) * 3);
        CHECK(md < 1e-4); // HONESTY GATE: same transmittance (FMA-fusion ULP aside), so the ms comparison is real
        std::printf("[crush] atmos transmittance  CKIR %7.3f ms | hand %7.3f ms | CKIR/hand %.3f | out-match %.1e  (%dx%d LUT)\n", msc, msh, msc / msh, md, w, h);
    }
    std::printf("================================================================\n");
}

TEST_CASE("B14-c: CKIR SVGF TEMPORAL integration DISPATCHES on Vulkan == CPU oracle (bit-exact -- no transcendentals)",
          "[gpu-context][vulkan][gpu][kernel][svgf]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::SvgfConfig           scfg;
    scfg.asvgf = true; // exercise the A-SVGF adaptive-α branch (sqrt/abs/div/min/max) on the GPU too
    kir::KGraph               g(&alloc);
    const kir::KEntry         e  = kir::build_svgf_temporal(g, scfg);
    const int                 np = scfg.width * scfg.height;

    // a VALID-history frame (prev geometry matches ⇒ reprojection + blend exercised), zero motion, noisy current.
    crd::containers::Array<crd::f64> cc(&alloc);
    crd::containers::Array<crd::f64> cg(&alloc);
    crd::containers::Array<crd::f64> mv(&alloc);
    crd::containers::Array<crd::f64> pc(&alloc);
    crd::containers::Array<crd::f64> ps(&alloc);
    crd::containers::Array<crd::f64> pg(&alloc);
    crd::containers::Array<crd::f64> oc(&alloc);
    crd::containers::Array<crd::f64> os(&alloc);
    cc.resize(uz(np * 3));
    cg.resize(uz(np * 4));
    mv.resize(uz(np * 2));
    pc.resize(uz(np * 3));
    ps.resize(uz(np * 4));
    pg.resize(uz(np * 4));
    oc.resize(uz(np * 3));
    os.resize(uz(np * 4));
    crd::u32 s = 3U;
    auto rnd = [&]() { s = s * 1664525U + 1013904223U; return static_cast<double>(s >> 8) / static_cast<double>(1U << 24); };
    for (int p = 0; p < np; ++p)
    {
        for (int c = 0; c < 3; ++c) { cc[uz(p * 3 + c)] = 0.4 + 0.3 * (rnd() - 0.5); pc[uz(p * 3 + c)] = 0.5; }
        cg[uz(p * 4 + 0)] = 1.0; cg[uz(p * 4 + 3)] = 1.0;
        pg[uz(p * 4 + 0)] = 1.0; pg[uz(p * 4 + 3)] = 1.0;
        ps[uz(p * 4 + 0)] = 0.5; ps[uz(p * 4 + 1)] = 0.26; ps[uz(p * 4 + 2)] = 5.0; ps[uz(p * 4 + 3)] = 0.01; // m1,m2,hist,var
    }
    kir::KernelBuffer bufs[8] = {{cc.data(), np * 3, 0, 0}, {cg.data(), np * 4, 0, 1}, {mv.data(), np * 2, 0, 2},
                                 {pc.data(), np * 3, 0, 3}, {ps.data(), np * 4, 0, 4}, {pg.data(), np * 4, 0, 5},
                                 {oc.data(), np * 3, 0, 6}, {os.data(), np * 4, 0, 7}};
    kir::eval_cpu_kernel(g, e, bufs, 8, e.local_size[0], &alloc, static_cast<crd::u32>(np / 64));

    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ckir_svgf_temporal", &alloc);
    REQUIRE(spv.ok);
    auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 8, 0U);
    REQUIRE(pipe != nullptr);

    const int lens[8] = {np * 3, np * 4, np * 2, np * 3, np * 4, np * 4, np * 3, np * 4};
    crd::containers::Array<float> h[8];
    float*                        host[8];
    for (int b = 0; b < 8; ++b) { h[b] = crd::containers::Array<float>(&alloc); h[b].resize(uz(lens[b])); host[b] = h[b].data(); }
    for (int b = 0; b < 6; ++b) { for (int i = 0; i < lens[b]; ++i) { h[b][uz(i)] = static_cast<float>(bufs[b].data[i]); } }
    for (int i = 0; i < lens[6]; ++i) { h[6][uz(i)] = -9.0F; }
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 8, static_cast<crd::u32>(np / 64));

    double maxad = 0.0;
    for (int i = 0; i < lens[6]; ++i) { const double d = std::fabs(static_cast<double>(h[6][uz(i)]) - oc[uz(i)]); if (d > maxad) { maxad = d; } }
    for (int i = 0; i < lens[7]; ++i) { const double d = std::fabs(static_cast<double>(h[7][uz(i)]) - os[uz(i)]); if (d > maxad) { maxad = d; } }
    std::printf("[Vulkan SVGF temporal] max_abs(GPU vs oracle) = %.2e\n", maxad);
    CHECK(maxad < 1e-5); // pure arithmetic (lerp + moments) ⇒ bit-close on every backend
}

TEST_CASE("v17 NRC: CKIR fused-MLP BACKWARD (dz chain + DETERMINISTIC dW) DISPATCHES on Vulkan == oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][mlp]")
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
    kir::MlpConfig             mcfg;
    mcfg.batch_tile = 64;
    mcfg.warps      = 2;
    const int wd    = mcfg.width;
    const int nl    = mcfg.layers;
    const int batch = 16;
    const int bw    = batch * wd;
    const int n_a   = (nl + 1) * bw;
    const int n_w   = nl * wd * wd;
    const int n_dz  = nl * bw;
    const int n_dw  = nl * wd * wd;

    // host forward storing all activations + the deterministic backward oracle
    crd::containers::Array<float> a_all(&alloc);
    a_all.resize(static_cast<crd::usize>(n_a));
    crd::containers::Array<float> w_f(&alloc);
    w_f.resize(static_cast<crd::usize>(n_w));
    for (int i = 0; i < n_w; ++i) { w_f[static_cast<crd::usize>(i)] = 0.1F * static_cast<float>((i * 5) % 11 - 5); }
    for (int r = 0; r < batch; ++r)
    {
        for (int c = 0; c < wd; ++c)
        {
            const int ai            = r * wd + c;
            a_all[static_cast<crd::usize>(ai)] = 0.2F * static_cast<float>(ai % 13 - 6);
        }
        for (int l = 0; l < nl; ++l)
        {
            for (int n = 0; n < wd; ++n)
            {
                float z = 0.0F;
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
    crd::containers::Array<float> gout(&alloc);
    gout.resize(static_cast<crd::usize>(bw));
    for (int i = 0; i < bw; ++i)
    {
        const int gi                     = nl * bw + i;
        gout[static_cast<crd::usize>(i)] = a_all[static_cast<crd::usize>(gi)];
    }
    crd::containers::Array<float> ref_dz(&alloc);
    ref_dz.resize(static_cast<crd::usize>(n_dz));
    crd::containers::Array<float> ref_dw(&alloc);
    ref_dw.resize(static_cast<crd::usize>(n_dw));
    crd::containers::Array<float> gs(&alloc);
    gs.resize(static_cast<crd::usize>(wd));
    crd::containers::Array<float> ngs(&alloc);
    ngs.resize(static_cast<crd::usize>(wd));
    kir::mlp_backward_ref(mcfg, a_all.data(), w_f.data(), gout.data(), batch, ref_dz.data(), ref_dw.data(), gs.data(), ngs.data());

    // Kernel A: dz chain → dz_all (grid = batch)
    kir::KGraph       g_a(&alloc);
    const kir::KEntry e_a = kir::build_mlp_bwd_dz(g_a, mcfg, batch);
    kir::GlslKernel   k_a(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g_a, e_a, &alloc, k_a));
    const auto spv_a = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k_a.source), "ckir_bwd_dz", &alloc);
    REQUIRE(spv_a.ok);
    auto pipe_a = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv_a.spirv.data(), spv_a.spirv.size()), 4, 0U);
    REQUIRE(pipe_a != nullptr);
    crd::containers::Array<float> dz(&alloc);
    dz.resize(static_cast<crd::usize>(n_dz));
    for (int i = 0; i < n_dz; ++i) { dz[static_cast<crd::usize>(i)] = -7.0F; }
    float*    host_a[4] = {a_all.data(), w_f.data(), gout.data(), dz.data()};
    const int lens_a[4] = {n_a, n_w, bw, n_dz};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe_a, host_a, lens_a, 4, static_cast<crd::u32>(batch));
    int bad_dz = 0;
    for (int i = 0; i < n_dz; ++i) { if (dz[static_cast<crd::usize>(i)] != ref_dz[static_cast<crd::usize>(i)]) { ++bad_dz; } }
    CHECK(bad_dz == 0);

    // Kernel B: DETERMINISTIC dW reduction (grid = L*W), using kernel A's dz
    kir::KGraph       g_b(&alloc);
    const kir::KEntry e_b = kir::build_mlp_bwd_dw(g_b, mcfg, batch);
    kir::GlslKernel   k_b(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g_b, e_b, &alloc, k_b));
    const auto spv_b = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(k_b.source), "ckir_bwd_dw", &alloc);
    REQUIRE(spv_b.ok);
    auto pipe_b = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv_b.spirv.data(), spv_b.spirv.size()), 3, 0U);
    REQUIRE(pipe_b != nullptr);
    crd::containers::Array<float> dw(&alloc);
    dw.resize(static_cast<crd::usize>(n_dw));
    crd::containers::Array<float> dw2(&alloc);
    dw2.resize(static_cast<crd::usize>(n_dw));
    const auto run_dw = [&](crd::containers::Array<float>& out) {
        for (int i = 0; i < n_dw; ++i) { out[static_cast<crd::usize>(i)] = -7.0F; }
        float*    host_b[3] = {a_all.data(), dz.data(), out.data()};
        const int lens_b[3] = {n_a, n_dz, n_dw};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe_b, host_b, lens_b, 3, static_cast<crd::u32>(nl * wd));
    };
    run_dw(dw);
    run_dw(dw2);
    int bad_dw = 0;
    int det    = 0;
    for (int i = 0; i < n_dw; ++i)
    {
        if (dw[static_cast<crd::usize>(i)] != ref_dw[static_cast<crd::usize>(i)]) { ++bad_dw; }
        if (dw[static_cast<crd::usize>(i)] != dw2[static_cast<crd::usize>(i)]) { ++det; }
    }
    CHECK(bad_dw == 0); // dW bit-exact vs the deterministic oracle
    CHECK(det == 0);    // run-to-run BIT-IDENTICAL — the moat the standalone's fp32-atomic dW could not hold
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

// B16-a-2: the BATCHED strided inverse 2-D FFT (build_fft2d_c2c_batched — 2 dispatches: batched row IFFT -> batched strided+
// tiled column IFFT) that the FFT-ocean's multi-field transform rides. Runs on Vulkan and is compared BIT-FOR-BIT to the CPU
// oracle driving the SAME plan across ALL batch images — validating the new radix-16 batched-strided column path on real HW.
TEST_CASE("B16-a-2: CKIR BATCHED strided inverse 2-D FFT DISPATCHES on Vulkan == CPU oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][fft][ocean]")
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
    kir::KGraph                g0(&alloc); // row FFT
    kir::KGraph                g1(&alloc); // strided column FFT
    kir::KGraph*               graphs[2] = {&g0, &g1};
    constexpr int              n         = 64; // power of FOUR
    constexpr int              batch     = 4;  // the FFT-ocean's 4 packed complex fields
    constexpr int              tile_c    = 8;
    constexpr int              rc        = n * n;
    const kir::Fft2dPlan       plan      = kir::build_fft2d_c2c_batched(graphs, n, n, batch, /*inverse=*/true, tile_c);

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
    for (int i = 0; i < rc * batch; ++i)
    {
        h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5); // integers -> f32-exact inputs
        h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 13 - 6);
    }
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        h64[plan.tw_col_re][k] = f32d(crd::math::cos(a));
        h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a));
        h64[plan.tw_row_re][k] = h64[plan.tw_col_re][k];
        h64[plan.tw_row_im][k] = h64[plan.tw_col_im][k];
    }
    for (int i = 0; i < total; ++i) { a32[static_cast<crd::usize>(i)] = static_cast<float>(a64[static_cast<crd::usize>(i)]); }

    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

    std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
    crd::gpu::ComputePipeline*                 pipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "fft2db", &alloc);
        REQUIRE(spv.ok);
        pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()),
                                                            plan.passes[pi].nbind, 0U);
        REQUIRE(pipe_store[pi] != nullptr);
        pipes[pi] = pipe_store[pi].get();
    }

    crd::kir_test::dispatch_fft2d(compute, plan, pipes, h32);

    const auto fa   = [](float x) { return x < 0.0F ? -x : x; };
    int        badr = 0;
    int        badi = 0;
    float      md   = 0.0F;
    for (int i = 0; i < rc * batch; ++i)
    {
        const float er = static_cast<float>(h64[plan.res_re][i]);
        const float ei = static_cast<float>(h64[plan.res_im][i]);
        if (h32[plan.res_re][i] != er) { ++badr; }
        if (h32[plan.res_im][i] != ei) { ++badi; }
        md = md > fa(h32[plan.res_re][i] - er) ? md : fa(h32[plan.res_re][i] - er);
        md = md > fa(h32[plan.res_im][i] - ei) ? md : fa(h32[plan.res_im][i] - ei);
    }
    WARN("[ocean-ifft-vk] " << n << "x" << n << " x" << batch << " bit-exact re-bad " << badr << " im-bad " << badi << " maxdiff " << md);
    CHECK(badr == 0);
    CHECK(badi == 0);
}

// B16-a-2: the FULL FFT-ocean a-2 pipeline on Vulkan — time-evolution -> batched inverse 2-D FFT -> assemble (displacement/
// normal/foam) — every stage dispatched + chained on the GPU, then the displacement + normal maps compared to the CPU oracle
// running the SAME graphs. Seeded from a CPU h0 (a-1 is CPU-verified) to isolate the a-2 kernels. The FFT is bit-exact; the
// per-texel transcendentals (e^{iωt}, dispersion, the sqrt/div in assemble) are hardware-vs-libm, so the chain matches to ULP.
TEST_CASE("B16-a-2: FULL FFT-ocean pipeline (evolve->IFFT->assemble) DISPATCHES on Vulkan == CPU oracle (ULP)",
          "[gpu-context][vulkan][gpu][kernel][ocean]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg2;
    cfg2.backend  = gpu::GpuBackend::Vulkan;
    cfg2.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(cfg2);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto*                          vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(192U << 20U);
    kir::ocean::OceanConfig    oc;
    oc.n            = 64; // power of four
    oc.patch_length = crd::units::Length64{250.0};
    oc.choppiness   = 1.0;
    oc.foam_bias    = 0.4;
    oc.foam_scale   = 2.0;
    const int          n      = oc.n;
    const int          rc     = n * n;
    const float        tval   = 2.0F;
    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    kir::KGraph       gspec(&alloc);
    kir::KGraph       gevo(&alloc);
    kir::KGraph       gasm(&alloc);
    kir::KGraph       grow(&alloc);
    kir::KGraph       gcol(&alloc);
    const kir::KEntry espec = kir::ocean::build_ocean_spectrum(gspec, oc);
    const kir::KEntry eevo  = kir::ocean::build_ocean_evolve(gevo, oc);
    const kir::KEntry easm  = kir::ocean::build_ocean_assemble(gasm, oc);
    kir::KGraph*         fftg[2] = {&grow, &gcol};
    const kir::Fft2dPlan plan    = kir::build_fft2d_c2c_batched(fftg, n, n, 4, true, 8);

    // --- shared input: a-1 spectrum on the CPU (a-1 is CPU-verified; both sides use the same f32 h0) ---
    crd::containers::Array<crd::f64> h0(&alloc);
    crd::containers::Array<crd::f64> ampd(&alloc);
    h0.resize(uz(rc * 4));
    ampd.resize(uz(rc));
    kir::KernelBuffer sb[2] = {{h0.data(), rc * 4, 0, 0}, {ampd.data(), rc, 0, 1}};
    kir::eval_cpu_kernel(gspec, espec, sb, 2, espec.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // --- CPU ORACLE: evolve -> IFFT -> assemble in f64 (eval_cpu_kernel rounds each op to f32) ---
    crd::containers::Array<crd::f64> pard(&alloc);
    crd::containers::Array<crd::f64> sro(&alloc);
    crd::containers::Array<crd::f64> sio(&alloc);
    pard.resize(1U, static_cast<crd::f64>(tval));
    sro.resize(uz(rc * 4), 0.0);
    sio.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer eb[4] = {{h0.data(), rc * 4, 0, 0}, {pard.data(), 1, 0, 1}, {sro.data(), rc * 4, 0, 2}, {sio.data(), rc * 4, 0, 3}};
    kir::eval_cpu_kernel(gevo, eevo, eb, 4, eevo.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(uz(total), 0.0);
    crd::f64* h64[16];
    for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = arena.data() + off[b]; }
    for (int i = 0; i < rc * 4; ++i) { h64[plan.in_re][i] = sro[uz(i)]; h64[plan.in_im][i] = sio[uz(i)]; }
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        h64[plan.tw_col_re][k] = f32d(crd::math::cos(a));
        h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a));
        h64[plan.tw_row_re][k] = h64[plan.tw_col_re][k];
        h64[plan.tw_row_im][k] = h64[plan.tw_col_im][k];
    }
    crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);
    crd::containers::Array<crd::f64> dispo(&alloc);
    crd::containers::Array<crd::f64> normo(&alloc);
    dispo.resize(uz(rc * 4), 0.0);
    normo.resize(uz(rc * 4), 0.0);
    kir::KernelBuffer ao[4] = {{h64[plan.res_re], rc * 4, 0, 0}, {h64[plan.res_im], rc * 4, 0, 1}, {dispo.data(), rc * 4, 0, 2}, {normo.data(), rc * 4, 0, 3}};
    kir::eval_cpu_kernel(gasm, easm, ao, 4, easm.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

    // --- GPU: emit + dispatch each stage, chaining host buffers ---
    const auto make_pipe = [&](kir::KGraph& gg, const kir::KEntry& ee, int nb, const char* nm) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(gg, ee, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), nm, &alloc);
        REQUIRE(spv.ok);
        auto pp = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
        REQUIRE(pp != nullptr);
        return pp;
    };

    // (1) evolve
    auto                          pevo = make_pipe(gevo, eevo, 4, "oevo");
    crd::containers::Array<float> h0f(&alloc);
    crd::containers::Array<float> parf(&alloc);
    crd::containers::Array<float> srf(&alloc);
    crd::containers::Array<float> sif(&alloc);
    h0f.resize(uz(rc * 4));
    parf.resize(1U, tval);
    srf.resize(uz(rc * 4), 0.0F);
    sif.resize(uz(rc * 4), 0.0F);
    for (int i = 0; i < rc * 4; ++i) { h0f[uz(i)] = static_cast<float>(h0[uz(i)]); }
    float*    eh[4] = {h0f.data(), parf.data(), srf.data(), sif.data()};
    const int el[4] = {rc * 4, 1, rc * 4, rc * 4};
    crd::kir_test::dispatch_kernel_1wg(compute, *pevo, eh, el, 4, static_cast<crd::u32>(rc / 64));

    // (2) batched IFFT (fill in_re/in_im from the evolve GPU output; twiddles f32)
    crd::containers::Array<float> a32(&alloc);
    a32.resize(uz(total), 0.0F);
    float* h32[16];
    for (int b = 0; b < plan.nbuffers; ++b) { h32[b] = a32.data() + off[b]; }
    for (int i = 0; i < rc * 4; ++i) { h32[plan.in_re][i] = srf[uz(i)]; h32[plan.in_im][i] = sif[uz(i)]; }
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        h32[plan.tw_col_re][k] = static_cast<float>(crd::math::cos(a));
        h32[plan.tw_col_im][k] = static_cast<float>(-crd::math::sin(a));
        h32[plan.tw_row_re][k] = h32[plan.tw_col_re][k];
        h32[plan.tw_row_im][k] = h32[plan.tw_col_im][k];
    }
    std::unique_ptr<crd::gpu::ComputePipeline> ifpipe[8];
    crd::gpu::ComputePipeline*                 ifpipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        ifpipe[pi]  = make_pipe(*plan.passes[pi].graph, plan.passes[pi].entry, plan.passes[pi].nbind, "oifft");
        ifpipes[pi] = ifpipe[pi].get();
    }
    crd::kir_test::dispatch_fft2d(compute, plan, ifpipes, h32);

    // (3) assemble
    auto                          pasm = make_pipe(gasm, easm, 4, "oasm");
    crd::containers::Array<float> dgf(&alloc);
    crd::containers::Array<float> ngf(&alloc);
    dgf.resize(uz(rc * 4), 0.0F);
    ngf.resize(uz(rc * 4), 0.0F);
    float*    ah[4] = {h32[plan.res_re], h32[plan.res_im], dgf.data(), ngf.data()};
    const int al[4] = {rc * 4, rc * 4, rc * 4, rc * 4};
    crd::kir_test::dispatch_kernel_1wg(compute, *pasm, ah, al, 4, static_cast<crd::u32>(rc / 64));

    // --- compare displacement + normal maps: GPU vs oracle, ULP (transcendental chain) ---
    double maxrel = 0.0;
    for (int i = 0; i < rc * 4; ++i)
    {
        const double sd = std::max(1.0, std::fabs(dispo[uz(i)]));
        const double sn = std::max(1.0, std::fabs(normo[uz(i)]));
        maxrel          = std::max(maxrel, std::fabs(static_cast<double>(dgf[uz(i)]) - dispo[uz(i)]) / sd);
        maxrel          = std::max(maxrel, std::fabs(static_cast<double>(ngf[uz(i)]) - normo[uz(i)]) / sn);
    }
    std::printf("[Vulkan FFT-ocean pipeline] maxrel(GPU vs oracle) = %.2e\n", maxrel);
    CHECK(maxrel < 2e-3);
}

// B16-a-4: the WATER SURFACE actually RENDERS on Vulkan — a fullscreen water quad (normal tilting with FragCoord so the
// Fresnel + sun glint sweep across the frame) drawn through `water_shade` behind the create_program seam. Readback proves it
// renders: a bluish deep-water body + a bright sun glint. Drawn from the SHARED build_water_fs, so the DX12 test draws the
// identical shading (the fragment fan-out proof).
TEST_CASE("B16-a-4: water_shade RENDERS on Vulkan (bluish body + sun glint)", "[gpu-context][vulkan][gpu][raster][ir][ocean]")
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

    constexpr crd::u32 dim = 64U;
    kir::KGraph        vg(&alloc);
    kir::KEntry        ve;
    crd::gputest::build_fullscreen_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_water_fs(fg, fe, dim);

    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    REQUIRE(program->valid());
    auto target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);

    const crd::u32 c  = target->read_pixel(dim / 2U, dim / 2U);
    const int      cr = static_cast<int>(c & 0xFFU);
    const int      cg = static_cast<int>((c >> 8U) & 0xFFU);
    const int      cb = static_cast<int>((c >> 16U) & 0xFFU);
    int            maxlum = 0;
    for (crd::u32 y = 0; y < dim; ++y)
    {
        for (crd::u32 x = 0; x < dim; ++x)
        {
            const crd::u32 p   = target->read_pixel(x, y);
            const int      lum = static_cast<int>(p & 0xFFU) + static_cast<int>((p >> 8U) & 0xFFU) + static_cast<int>((p >> 16U) & 0xFFU);
            if (lum > maxlum) { maxlum = lum; }
        }
    }
    WARN("[water-render-vk] centre RGB=(" << cr << "," << cg << "," << cb << ") maxlum=" << maxlum);
    CHECK(cb > cr);                    // deep water reads bluish (blue channel dominant)
    CHECK(cb > 10);                    // and it actually rendered (not black)
    CHECK(maxlum > (cr + cg + cb) + 90); // a bright sun glint exists somewhere, well above the body
}

// B16-a-4: RENDER real ocean frames to BMP files you can open. A perspective sea to the horizon (build_ocean_frame_fs) at a
// few animation times, drawn on Vulkan and written to D:/Dev/cerid/build/ocean_frame_*.bmp. Hidden ([.ocean-frame]) — run it,
// then open the BMPs. Prints per-frame mean RGB + peak luminance so the render is sanity-checked even without eyes on it.
TEST_CASE("B16-a-4: RENDER ocean frames to BMP (open them!)", "[.ocean-frame]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("adapter has no VK_EXT_shader_object; skipping"); return; }
    crd::memory::TlsfAllocator     alloc(384U << 20U);
    crd::gpu::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    constexpr crd::u32 dim     = 512U;
    constexpr crd::u32 ss      = 3U;        // SUPERSAMPLE factor — render ss×dim then box-downfilter to dim (reference AA — 3× = 9 samples/px)
    const crd::u32     rdim    = dim * ss;  // supersampled render resolution
    const crd::u32     rowsize = (dim * 3U + 3U) & ~3U;
    const auto         uz      = [](int v) { return static_cast<crd::usize>(v); };

    // LEVER 4: bake OUR FFT-ocean field on the GPU (spectrum→evolve(t)→batched IFFT→assemble — the actual CKIR GPU pipeline the
    // B16-a-2 test proves bit-exact) into an RGBA8 tile (R,G = normal.xz · B = height · A = Jacobian foam); the raymarch samples it.
    constexpr int      on         = 256; // FFT tile resolution (power of four)
    const int          otc        = on * on;
    constexpr crd::f64 two_pi     = 6.28318530717958647693;

    // ── FOUR cascades (the production 4-spectrum ocean): descending world scales, band-limited so each owns a wavelength band.
    //    The BIG cascades carry high-amplitude rolling-swell GEOMETRY; the FINE cascades are low-amplitude detail (mostly in the
    //    per-pixel normal). Four NON-HARMONIC tile sizes push the visible tiling period to their LCM ⇒ effectively non-repeating.
    //    Foam comes from the SUM of the per-cascade Jacobian folds (the joint folding of the combined 4-spectrum surface). ──
    constexpr int                    nc = 4;
    crd::gputest::OceanCascadeRender  ocr; // render params (world scales + geo/normal weights + joint-foam thresholds), shared VS+FS
    ocr.count      = nc;
    ocr.patch[0]   = 160.0; // GENTLE VAST sea: big low-freq rollers dominate the geometry, deep-water tiling pushed far out
    ocr.patch[1]   = 72.0;
    ocr.patch[2]   = 31.0;
    ocr.patch[3]   = 13.0;
    // amplitude split (user): LOW freq = tall geometry, HIGH freq = normal-map texture. Fine cascades barely displace the mesh.
    ocr.geo_w[0]   = 1.00; ocr.geo_w[1] = 0.42; ocr.geo_w[2] = 0.12; ocr.geo_w[3] = 0.035;
    ocr.nrm_w[0]   = 0.95; ocr.nrm_w[1] = 1.00; ocr.nrm_w[2] = 0.85; ocr.nrm_w[3] = 0.55; // fine detail lives in the normal
    ocr.foam_w[0]  = 0.9;  ocr.foam_w[1] = 1.0;  ocr.foam_w[2] = 1.0;  ocr.foam_w[3] = 0.8;
    ocr.foam_bias  = 0.14; // the baked A is ACCUMULATED temporal foam (0..1 coverage) — threshold to crest whitecaps, clean troughs
    ocr.foam_scale = 1.8;

    kir::ocean::OceanConfig cfgs[nc];
    for (int c = 0; c < nc; ++c) { cfgs[c].n = on; }
    // 0 = big directional SWELL · 1 = mid · 2 = chop · 3 = fine ripple. `small_wave` BAND-LIMITS each cascade's short waves so the
    // bands don't overlap. GENTLE sea — lower wind (less amplitude) but enough choppiness on 1/2 that crests still pinch → foam.
    // foam_bias/scale = the assemble kernel's PER-STEP injection coverage; foam_decay = how long a whitecap LINGERS (temporal).
    // GENTLE geometry (low wind/amplitude) but HIGH choppiness on the mid/fine cascades — choppiness sharpens only the baked
    // normal + Jacobian (the geometry is vertical-only), so it makes the crests BREAK (foam) without steepening the gentle mesh.
    cfgs[0].patch_length = crd::units::Length64{520.0}; cfgs[0].wind_speed = crd::units::Velocity64{8.5}; cfgs[0].fetch = crd::units::Length64{300000.0}; cfgs[0].small_wave = crd::units::Length64{9.0};  cfgs[0].swell = 0.72; cfgs[0].choppiness = 0.60;
    cfgs[1].patch_length = crd::units::Length64{250.0}; cfgs[1].wind_speed = crd::units::Velocity64{7.5}; cfgs[1].fetch = crd::units::Length64{60000.0};  cfgs[1].small_wave = crd::units::Length64{3.2};  cfgs[1].swell = 0.40; cfgs[1].choppiness = 1.30;
    cfgs[2].patch_length = crd::units::Length64{110.0}; cfgs[2].wind_speed = crd::units::Velocity64{6.5}; cfgs[2].fetch = crd::units::Length64{14000.0};  cfgs[2].small_wave = crd::units::Length64{0.9};  cfgs[2].swell = 0.22; cfgs[2].choppiness = 1.50;
    cfgs[3].patch_length = crd::units::Length64{46.0};  cfgs[3].wind_speed = crd::units::Velocity64{5.5}; cfgs[3].fetch = crd::units::Length64{3500.0};   cfgs[3].small_wave = crd::units::Length64{0.22}; cfgs[3].swell = 0.12; cfgs[3].choppiness = 1.35;
    for (int c = 0; c < nc; ++c)
    {
        cfgs[c].foam_bias  = 0.70;                // inject foam at pinching crests (J < 0.7) → whitecaps on the crests
        cfgs[c].foam_scale = 1.6;
        cfgs[c].foam_decay = 0.965;               // whitecaps linger then fade exponentially (never instant) — the user's model
    }

    kir::KGraph          grow(&alloc);
    kir::KGraph          gcol(&alloc);
    kir::KGraph*         fftg[2] = {&grow, &gcol};
    const kir::Fft2dPlan plan    = kir::build_fft2d_c2c_batched(fftg, on, on, 4, true, 8);

    const auto make_pipe = [&](kir::KGraph& gg, const kir::KEntry& ee, int nb, const char* nm) {
        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(gg, ee, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), nm, &alloc);
        REQUIRE(spv.ok);
        auto pp = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
        REQUIRE(pp != nullptr);
        return pp;
    };
    std::unique_ptr<crd::gpu::ComputePipeline> ifpipe[8];
    crd::gpu::ComputePipeline*                 ifpipes[8] = {};
    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        ifpipe[pi]  = make_pipe(*plan.passes[pi].graph, plan.passes[pi].entry, plan.passes[pi].nbind, "oifft");
        ifpipes[pi] = ifpipe[pi].get();
    }

    // per-cascade evolve + assemble pipelines + the time-independent spectrum h0 (dispatched once now). The spectrum graph is
    // transient — the compiled pipeline owns its shader — so it lives only inside the loop. h0 for all 4 cascades is packed into
    // one buffer (cascade c at offset c·otc·4).
    std::unique_ptr<crd::gpu::ComputePipeline> pevo[nc];
    std::unique_ptr<crd::gpu::ComputePipeline> pasm[nc];
    crd::containers::Array<float>              h0all(&alloc);
    crd::containers::Array<float>              ampsc(&alloc);
    h0all.resize(uz(nc * otc * 4), 0.0F);
    ampsc.resize(uz(otc), 0.0F);
    for (int c = 0; c < nc; ++c)
    {
        kir::KGraph       gs(&alloc);
        kir::KGraph       ge(&alloc);
        kir::KGraph       ga(&alloc);
        const kir::KEntry es = kir::ocean::build_ocean_spectrum(gs, cfgs[c]);
        const kir::KEntry ee = kir::ocean::build_ocean_evolve(ge, cfgs[c]);
        const kir::KEntry ea = kir::ocean::build_ocean_assemble(ga, cfgs[c]);
        auto              ps = make_pipe(gs, es, 2, "ospec");
        pevo[c]              = make_pipe(ge, ee, 4, "oevo");
        pasm[c]              = make_pipe(ga, ea, 4, "oasm");
        float*    shp[2] = {h0all.data() + uz(c * otc * 4), ampsc.data()};
        const int slp[2] = {otc * 4, otc};
        crd::kir_test::dispatch_kernel_1wg(compute, *ps, shp, slp, 2, static_cast<crd::u32>(otc / 64));
    }

    // TEMPORAL FOAM accumulation kernel (shared — all cascades share foam_decay): foam(t) = max(foam(t−1)·decay, inject(t)).
    // When the Jacobian goes positive again (no breaking, inject≈0) the foam DECAYS exponentially (×decay) — never instantly.
    kir::KGraph       gfoam(&alloc);
    const kir::KEntry efoam = kir::ocean::build_ocean_foam_accumulate(gfoam, cfgs[0]);
    auto              pfoam = make_pipe(gfoam, efoam, 3, "ofoam");

    // FFT working arena (f32) — fixed twiddles filled once; evolve fills in_re/in_im per frame.
    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<float> a32(&alloc);
    a32.resize(uz(total), 0.0F);
    float* h32[16];
    for (int b = 0; b < plan.nbuffers; ++b) { h32[b] = a32.data() + off[b]; }
    for (int kk = 0; kk < on; ++kk)
    {
        const crd::f64 a        = two_pi * static_cast<crd::f64>(kk) / static_cast<crd::f64>(on);
        h32[plan.tw_col_re][kk] = static_cast<float>(crd::math::cos(a));
        h32[plan.tw_col_im][kk] = static_cast<float>(-crd::math::sin(a));
        h32[plan.tw_row_re][kk] = h32[plan.tw_col_re][kk];
        h32[plan.tw_row_im][kk] = h32[plan.tw_col_im][kk];
    }

    // Bake cascade `c` at time t → its RGBA8 field [nx, nz, height/(2·hmax), ACCUMULATED foam]. A WARMUP window steps the sim to
    // time t, and each step accumulates temporal foam foam(s)=max(foam(s−1)·decay, inject) — whitecaps form where the Jacobian
    // breaks (J<foam_bias), LINGER, then decay exponentially once the crest relaxes (never instant). Returns hmax (height scale).
    const auto bake_cascade = [&](int c, double t, unsigned char* rgba) -> double {
        crd::containers::Array<float> parf(&alloc);
        crd::containers::Array<float> srf(&alloc);
        crd::containers::Array<float> sif(&alloc);
        crd::containers::Array<float> dgf(&alloc);
        crd::containers::Array<float> ngf(&alloc);
        crd::containers::Array<float> inj(&alloc);
        crd::containers::Array<float> foam_a(&alloc);
        crd::containers::Array<float> foam_b(&alloc);
        parf.resize(1U, 0.0F);
        srf.resize(uz(otc * 4), 0.0F);
        sif.resize(uz(otc * 4), 0.0F);
        dgf.resize(uz(otc * 4), 0.0F);
        ngf.resize(uz(otc * 4), 0.0F);
        inj.resize(uz(otc), 0.0F);
        foam_a.resize(uz(otc), 0.0F);
        foam_b.resize(uz(otc), 0.0F);
        constexpr int    warm = 22;  // warmup steps — whitecaps accumulate over the window then linger + decay
        constexpr double dts  = 0.19;
        for (int s = 0; s <= warm; ++s)
        {
            parf[0]         = static_cast<float>(t - static_cast<double>(warm - s) * dts);
            float*    eh[4] = {h0all.data() + uz(c * otc * 4), parf.data(), srf.data(), sif.data()};
            const int el[4] = {otc * 4, 1, otc * 4, otc * 4};
            crd::kir_test::dispatch_kernel_1wg(compute, *pevo[c], eh, el, 4, static_cast<crd::u32>(otc / 64));
            for (int i = 0; i < otc * 4; ++i) { h32[plan.in_re][i] = srf[uz(i)]; h32[plan.in_im][i] = sif[uz(i)]; }
            crd::kir_test::dispatch_fft2d(compute, plan, ifpipes, h32);
            float*    ah[4] = {h32[plan.res_re], h32[plan.res_im], dgf.data(), ngf.data()};
            const int al[4] = {otc * 4, otc * 4, otc * 4, otc * 4};
            crd::kir_test::dispatch_kernel_1wg(compute, *pasm[c], ah, al, 4, static_cast<crd::u32>(otc / 64));
            for (int i = 0; i < otc; ++i) { inj[uz(i)] = dgf[uz(i * 4 + 3)]; } // this step's Jacobian breaking coverage
            float*    fh[3] = {foam_a.data(), inj.data(), foam_b.data()};
            const int fl[3] = {otc, otc, otc};
            crd::kir_test::dispatch_kernel_1wg(compute, *pfoam, fh, fl, 3, static_cast<crd::u32>(otc / 64));
            for (int i = 0; i < otc; ++i) { foam_a[uz(i)] = foam_b[uz(i)]; } // ping-pong; foam_a holds the accumulated foam
        }
        double hmax = 1e-3;
        for (int i = 0; i < otc; ++i) { hmax = std::max(hmax, static_cast<double>(std::fabs(dgf[uz(i * 4 + 1)]))); }
        const auto enc = [](double x) {
            long vv = std::lround(x * 255.0);
            if (vv < 0) { vv = 0; }
            if (vv > 255) { vv = 255; }
            return static_cast<unsigned char>(vv);
        };
        for (int i = 0; i < otc; ++i)
        {
            rgba[uz(i * 4 + 0)] = enc(static_cast<double>(ngf[uz(i * 4 + 0)]) * 0.5 + 0.5);          // nx
            rgba[uz(i * 4 + 1)] = enc(static_cast<double>(ngf[uz(i * 4 + 2)]) * 0.5 + 0.5);          // nz
            rgba[uz(i * 4 + 2)] = enc(static_cast<double>(dgf[uz(i * 4 + 1)]) / (2.0 * hmax) + 0.5); // height
            rgba[uz(i * 4 + 3)] = enc(static_cast<double>(foam_a[uz(i)]));                            // ACCUMULATED temporal foam
        }
        return hmax;
    };

    // GOD RAYS + BMP write, shared by the fragment and geometry deliverables. `px` is the display-resolution image (row 0 =
    // screen top). Marches each pixel toward the sun's screen position, accumulating the bright light it "sees" (crepuscular
    // shafts), then writes a 24-bit BMP to `path`.
    const auto finish = [&](const crd::containers::Array<crd::u32>& px, const char* path) {
        const double lx = 0.25;
        const double ly = 0.28;
        const double lz = 0.93;
        const double sux   = lx / (lz * 0.9);
        const double suy   = (ly / lz + 0.14) / 0.60;
        const double sunfx = (sux + 1.0) * static_cast<double>(dim) * 0.5;
        const double sunfy = (1.0 - suy) * static_cast<double>(dim) * 0.5;
        crd::containers::Array<crd::u32> out(&alloc);
        out.resize(static_cast<crd::usize>(dim) * dim);
        constexpr int gnum = 96;
        for (crd::u32 y = 0; y < dim; ++y)
        {
            for (crd::u32 x = 0; x < dim; ++x)
            {
                double       ar    = 0.0;
                double       ag    = 0.0;
                double       ab    = 0.0;
                double       decay = 1.0;
                const double dxs   = (sunfx - static_cast<double>(x)) / gnum;
                const double dys   = (sunfy - static_cast<double>(y)) / gnum;
                double       spx   = static_cast<double>(x);
                double       spy   = static_cast<double>(y);
                for (int i = 0; i < gnum; ++i)
                {
                    spx += dxs;
                    spy += dys;
                    const int ix = static_cast<int>(spx);
                    const int iy = static_cast<int>(spy);
                    if (ix < 0 || iy < 0 || ix >= static_cast<int>(dim) || iy >= static_cast<int>(dim)) { continue; }
                    const crd::u32 sp  = px[static_cast<crd::usize>(iy) * dim + static_cast<crd::u32>(ix)];
                    const double   rr  = static_cast<double>(sp & 0xFFU) / 255.0;
                    const double   gg  = static_cast<double>((sp >> 8U) & 0xFFU) / 255.0;
                    const double   bb  = static_cast<double>((sp >> 16U) & 0xFFU) / 255.0;
                    const double   lum = 0.3 * rr + 0.5 * gg + 0.2 * bb;
                    const double   m   = lum > 0.74 ? (lum - 0.74) * 2.8 : 0.0; // only the BRIGHTEST sun/foam casts shafts (no white-out)
                    const double   w   = decay * m;
                    ar += rr * w;
                    ag += gg * w;
                    ab += bb * w;
                    decay *= 0.977;
                }
                const double   gain = 0.7 / gnum; // visible crepuscular shafts, not a blown-out sky
                const crd::u32 p    = px[static_cast<crd::usize>(y) * dim + x];
                const auto     cl   = [](double c) {
                    long vv = std::lround(c * 255.0);
                    if (vv < 0) { vv = 0; }
                    if (vv > 255) { vv = 255; }
                    return static_cast<crd::u32>(vv);
                };
                const crd::u32 rc   = cl(static_cast<double>(p & 0xFFU) / 255.0 + ar * gain);
                const crd::u32 gc   = cl(static_cast<double>((p >> 8U) & 0xFFU) / 255.0 + ag * gain * 0.75);
                const crd::u32 bc   = cl(static_cast<double>((p >> 16U) & 0xFFU) / 255.0 + ab * gain * 0.5);
                out[static_cast<crd::usize>(y) * dim + x] = rc | (gc << 8U) | (bc << 16U) | 0xFF000000U;
            }
        }

        crd::containers::Array<unsigned char> bmp(&alloc);
        bmp.resize(54U + static_cast<crd::usize>(rowsize) * dim, static_cast<unsigned char>(0));
        const auto p4 = [&](crd::u32 o, crd::u32 vv) {
            bmp[o]     = static_cast<unsigned char>(vv & 0xFFU);
            bmp[o + 1] = static_cast<unsigned char>((vv >> 8U) & 0xFFU);
            bmp[o + 2] = static_cast<unsigned char>((vv >> 16U) & 0xFFU);
            bmp[o + 3] = static_cast<unsigned char>((vv >> 24U) & 0xFFU);
        };
        bmp[0] = 'B';
        bmp[1] = 'M';
        p4(2U, 54U + rowsize * dim);
        p4(10U, 54U);
        p4(14U, 40U);
        p4(18U, dim);
        p4(22U, dim);
        bmp[26] = 1U;
        bmp[28] = 24U;
        p4(34U, rowsize * dim);
        for (crd::u32 fy = 0; fy < dim; ++fy)
        {
            const crd::u32 sy = dim - 1U - fy;
            for (crd::u32 x = 0; x < dim; ++x)
            {
                const crd::u32 pv = out[static_cast<crd::usize>(sy) * dim + x];
                const crd::u32 o  = 54U + fy * rowsize + x * 3U;
                bmp[o]     = static_cast<unsigned char>((pv >> 16U) & 0xFFU);
                bmp[o + 1] = static_cast<unsigned char>((pv >> 8U) & 0xFFU);
                bmp[o + 2] = static_cast<unsigned char>(pv & 0xFFU);
            }
        }
        FILE* f = nullptr;
#ifdef _MSC_VER
        if (fopen_s(&f, path, "wb") != 0) { f = nullptr; } // MSVC: the deprecated fopen errors under /WX
#else
        f = std::fopen(path, "wb"); // fopen_s is MSVC-only (the hair_render.hpp idiom)
#endif
        if (f != nullptr)
        {
            fwrite(bmp.data(), 1U, bmp.size(), f);
            fclose(f);
        }
    };

    const auto frame = [&](double t, const char* path, const char* path_geo, const char* path_mesh) {
        // ── (A/B) bake ALL FOUR cascades on the GPU into one packed buffer → 4 mipmapped bindless textures; fill ocr.hmax[c] ──
        crd::containers::Array<unsigned char> rgball(&alloc);
        rgball.resize(uz(nc * otc * 4), static_cast<unsigned char>(0));
        for (int c = 0; c < nc; ++c) { ocr.hmax[c] = bake_cascade(c, t, rgball.data() + uz(c * otc * 4)); }
        std::unique_ptr<crd::gpu::ITexture> texc[nc];
        crd::gpu::ITexture*                 texs[8] = {};
        for (int c = 0; c < nc; ++c)
        {
            texc[c] = raster->create_texture_mipped(static_cast<crd::u32>(on), static_cast<crd::u32>(on), rgball.data() + uz(c * otc * 4));
            REQUIRE(texc[c] != nullptr);
            texs[c] = texc[c].get();
        }

        // ── (C) the FRAGMENT deliverable: a fullscreen pass that renders the SKY (its water, sampling cascades 0/1, is overwritten
        //    by the geometry pass — it only needs to supply the sky above the horizon + the "before" comparison image) ──
        kir::KGraph vg(&alloc);
        kir::KEntry ve;
        crd::gputest::build_fullscreen_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        crd::gputest::build_ocean_frame_fft_fs(fg, fe, rdim, ocr.hmax[0], ocr.hmax[1], ocr.patch[0], ocr.patch[1]);
        auto vs = ctx->create_program(vg, ve);
        auto fs = ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        REQUIRE(program->valid());
        auto target = raster->create_color_target(rdim, rdim);
        REQUIRE(target != nullptr);
        raster->draw_bindless(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, texs, 2U, 3U);

        // read the supersampled frame, then BOX-DOWNFILTER each ss×ss block → one display pixel (reference-quality AA).
        crd::containers::Array<crd::u32> hires(&alloc);
        hires.resize(static_cast<crd::usize>(rdim) * rdim);
        for (crd::u32 y = 0; y < rdim; ++y)
        {
            for (crd::u32 x = 0; x < rdim; ++x) { hires[static_cast<crd::usize>(y) * rdim + x] = target->read_pixel(x, y); }
        }
        crd::containers::Array<crd::u32> px(&alloc);
        px.resize(static_cast<crd::usize>(dim) * dim);
        for (crd::u32 y = 0; y < dim; ++y)
        {
            for (crd::u32 x = 0; x < dim; ++x)
            {
                crd::u32 ar = 0;
                crd::u32 ag = 0;
                crd::u32 ab = 0;
                for (crd::u32 sy = 0; sy < ss; ++sy)
                {
                    for (crd::u32 sx = 0; sx < ss; ++sx)
                    {
                        const crd::u32 hp = hires[static_cast<crd::usize>(y * ss + sy) * rdim + (x * ss + sx)];
                        ar += hp & 0xFFU;
                        ag += (hp >> 8U) & 0xFFU;
                        ab += (hp >> 16U) & 0xFFU;
                    }
                }
                const crd::u32 nsub = ss * ss;
                px[static_cast<crd::usize>(y) * dim + x] = (ar / nsub) | ((ag / nsub) << 8U) | ((ab / nsub) << 16U) | 0xFF000000U;
            }
        }
        double     sr = 0.0;
        double     sg = 0.0;
        double     sb = 0.0;
        crd::u32   mx = 0;
        for (crd::usize i = 0; i < px.size(); ++i)
        {
            const crd::u32 p = px[i];
            sr += static_cast<double>(p & 0xFFU);
            sg += static_cast<double>((p >> 8U) & 0xFFU);
            sb += static_cast<double>((p >> 16U) & 0xFFU);
            const crd::u32 lum = (p & 0xFFU) + ((p >> 8U) & 0xFFU) + ((p >> 16U) & 0xFFU);
            if (lum > mx) { mx = lum; }
        }
        const double np = static_cast<double>(dim) * dim;
        WARN("[ocean-frame] t=" << t << " mean RGB=(" << sr / np << "," << sg / np << "," << sb / np << ") maxlum=" << mx << " -> " << path);

        // ── the FRAGMENT deliverable (sky + per-pixel-normal water), kept for side-by-side comparison ──
        finish(px, path);

        // readback a supersampled DEPTH geometry pass (RGBA — alpha = coverage), box-downfilter, composite over the sky `px`,
        // and write. Shared by the vertex-pull geometry deliverable AND the mesh-shader deliverable (they render identically).
        const auto composite_over_sky = [&](gpu::IRasterTarget& gt, const char* out_path) {
            crd::containers::Array<crd::u32> ghi(&alloc);
            ghi.resize(static_cast<crd::usize>(rdim) * rdim);
            for (crd::u32 y = 0; y < rdim; ++y)
            {
                for (crd::u32 x = 0; x < rdim; ++x) { ghi[static_cast<crd::usize>(y) * rdim + x] = gt.read_pixel(x, y); }
            }
            crd::containers::Array<crd::u32> pxg(&alloc);
            pxg.resize(static_cast<crd::usize>(dim) * dim);
            for (crd::u32 y = 0; y < dim; ++y)
            {
                for (crd::u32 x = 0; x < dim; ++x)
                {
                    crd::u32 gr = 0;
                    crd::u32 gg = 0;
                    crd::u32 gb = 0;
                    crd::u32 ga = 0;
                    for (crd::u32 sy = 0; sy < ss; ++sy)
                    {
                        for (crd::u32 sx = 0; sx < ss; ++sx)
                        {
                            const crd::u32 hp = ghi[static_cast<crd::usize>(y * ss + sy) * rdim + (x * ss + sx)];
                            gr += hp & 0xFFU;
                            gg += (hp >> 8U) & 0xFFU;
                            gb += (hp >> 16U) & 0xFFU;
                            ga += (hp >> 24U) & 0xFFU;
                        }
                    }
                    const crd::u32 nsub = ss * ss;
                    const crd::u32 sky  = px[static_cast<crd::usize>(y) * dim + x];
                    const double   cov  = static_cast<double>(ga) / (static_cast<double>(nsub) * 255.0); // coverage 0..1
                    const auto     mix  = [&](crd::u32 gsum, crd::u32 sky8) {
                        long vv = std::lround(cov * (static_cast<double>(gsum) / nsub) + (1.0 - cov) * static_cast<double>(sky8));
                        if (vv < 0) { vv = 0; }
                        if (vv > 255) { vv = 255; }
                        return static_cast<crd::u32>(vv);
                    };
                    const crd::u32 cr = mix(gr, sky & 0xFFU);
                    const crd::u32 cg = mix(gg, (sky >> 8U) & 0xFFU);
                    const crd::u32 cb = mix(gb, (sky >> 16U) & 0xFFU);
                    pxg[static_cast<crd::usize>(y) * dim + x] = cr | (cg << 8U) | (cb << 16U) | 0xFF000000U;
                }
            }
            finish(pxg, out_path);
        };

        // ── (D) the DISPLACED-GEOMETRY deliverable — the GOLD-STANDARD path. Render the projected-grid ocean (real FFT-displaced
        //    geometry via the new SampleIndexedLod vertex fetch) into a DEPTH target, then composite it over the fragment SKY by
        //    coverage α. The geometry carries the swell silhouettes and the far field, so the "white noise" busy look is gone. ──
        constexpr int grid_n = 220; // cells/side → grid_n·grid_n·6 vertices (vertex-pull, no index buffer)
        kir::KGraph gvg(&alloc);
        kir::KEntry gve;
        crd::gputest::build_ocean_displaced_vs(gvg, gve, grid_n, ocr);
        kir::KGraph gfg(&alloc);
        kir::KEntry gfe;
        crd::gputest::build_ocean_water_geo_fs(gfg, gfe, ocr);
        auto gvs = ctx->create_program(gvg, gve);
        auto gfs = ctx->create_program(gfg, gfe);
        REQUIRE(gvs != nullptr);
        REQUIRE(gfs != nullptr);
        auto gprogram = raster->create_raster_program(*gvs, *gfs);
        REQUIRE(gprogram != nullptr);
        REQUIRE(gprogram->valid());
        auto gtarget = raster->create_color_depth_target(rdim, rdim);
        REQUIRE(gtarget != nullptr);
        const crd::u32 vcount = static_cast<crd::u32>(grid_n) * static_cast<crd::u32>(grid_n) * 6U;
        raster->draw_bindless_depth(*gtarget, *gprogram, gpu::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 1.0F, gpu::DepthCompare::Less,
                                    texs, static_cast<crd::u32>(nc), vcount);
        composite_over_sky(*gtarget, path_geo);

        // ── (E) the MESH-SHADER deliverable — the SAME projected-grid ocean emitted as MESHLETS (each workgroup a 12×12 patch;
        //    20×20 = 400 meshlets tile the grid). Reuses ocean_projected_vertex, so it renders pixel-identically to the vertex
        //    pull path — but as real meshlets (the GPU-driven / cullable geometry substrate). Only when the device has mesh. ──
        if (vk->mesh_shader() && path_mesh != nullptr)
        {
            constexpr int mnp = 32; // patches per side (32×7 = 224 lattice cells, ≈ the 220 vertex-pull grid)
            constexpr int mkk = 8;  // vertices per patch side: 8²=64 verts, 2·7²=98 tris ⇒ local_size 98 ≤ glslang's 128 mesh cap
            kir::KGraph   mmg(&alloc);
            kir::KEntry   mme;
            crd::gputest::build_ocean_displaced_mesh(mmg, mme, mnp, mkk, ocr);
            kir::KGraph mfg(&alloc);
            kir::KEntry mfe;
            crd::gputest::build_ocean_water_geo_fs(mfg, mfe, ocr);
            auto mmesh = ctx->create_program(mmg, mme);
            auto mfs   = ctx->create_program(mfg, mfe);
            REQUIRE(mmesh != nullptr);
            REQUIRE(mfs != nullptr);
            auto mprogram = raster->create_mesh_program(*mmesh, *mfs);
            REQUIRE(mprogram != nullptr);
            REQUIRE(mprogram->valid());
            auto mtarget = raster->create_color_depth_target(rdim, rdim);
            REQUIRE(mtarget != nullptr);
            raster->draw_mesh_bindless_depth(*mtarget, *mprogram, gpu::ClearColor{0.0F, 0.0F, 0.0F, 0.0F}, 1.0F,
                                             gpu::DepthCompare::Less, texs, static_cast<crd::u32>(nc),
                                             static_cast<crd::u32>(mnp * mnp)); // 400 meshlet workgroups
            composite_over_sky(*mtarget, path_mesh);
        }
    };

    frame(0.0, "D:/Dev/cerid/build/ocean_frame_0.bmp", "D:/Dev/cerid/build/ocean_geo_0.bmp", "D:/Dev/cerid/build/ocean_mesh_0.bmp");
    frame(2.5, "D:/Dev/cerid/build/ocean_frame_1.bmp", "D:/Dev/cerid/build/ocean_geo_1.bmp", "D:/Dev/cerid/build/ocean_mesh_1.bmp");
    frame(5.0, "D:/Dev/cerid/build/ocean_frame_2.bmp", "D:/Dev/cerid/build/ocean_geo_2.bmp", "D:/Dev/cerid/build/ocean_mesh_2.bmp");
    CHECK(true); // visual deliverable — ocean_frame_* = fragment · ocean_geo_* = vertex-pull geometry · ocean_mesh_* = mesh shaders
}

// B16-a-3: GPU-validate the multi-scale ocean's new surface on Vulkan — (A) the temporal foam-accumulation kernel BIT-EXACT
// (max/mul, no transcendentals), and (B) the multi-cascade batched inverse 2-D FFT at batch = 4·C (C=3 ⇒ 12 fields) BIT-EXACT
// vs the CPU oracle — proving the DRAM-bound multi-cascade batch runs correctly on real hardware.
TEST_CASE("B16-a-3: foam accumulate + multi-cascade batch-12 IFFT DISPATCH on Vulkan == oracle bit-exact",
          "[gpu-context][vulkan][gpu][kernel][ocean]")
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
    const auto uz = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator alloc(96U << 20U);
    constexpr int              n  = 64;
    constexpr int              rc = n * n;

    // ── (A) temporal foam accumulation, bit-exact ──
    {
        kir::ocean::OceanConfig oc;
        oc.n           = n;
        oc.foam_decay  = 0.9;
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::ocean::build_ocean_foam_accumulate(g, oc);

        crd::containers::Array<crd::f64> prev(&alloc);
        crd::containers::Array<crd::f64> inj(&alloc);
        crd::containers::Array<crd::f64> outo(&alloc);
        prev.resize(uz(rc));
        inj.resize(uz(rc));
        outo.resize(uz(rc), 0.0);
        for (int i = 0; i < rc; ++i) { prev[uz(i)] = 0.01 * static_cast<double>(i % 7); inj[uz(i)] = (i % 5 == 0) ? 0.7 : 0.0; }
        kir::KernelBuffer ob[3] = {{prev.data(), rc, 0, 0}, {inj.data(), rc, 0, 1}, {outo.data(), rc, 0, 2}};
        kir::eval_cpu_kernel(g, e, ob, 3, e.local_size[0], &alloc, static_cast<crd::u32>(rc / 64));

        kir::GlslKernel kern(&alloc);
        REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "ofoam", &alloc);
        REQUIRE(spv.ok);
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 0U);
        REQUIRE(pipe != nullptr);
        crd::containers::Array<float> hp(&alloc);
        crd::containers::Array<float> hi(&alloc);
        crd::containers::Array<float> ho(&alloc);
        hp.resize(uz(rc));
        hi.resize(uz(rc));
        ho.resize(uz(rc), -9.0F);
        for (int i = 0; i < rc; ++i) { hp[uz(i)] = static_cast<float>(prev[uz(i)]); hi[uz(i)] = static_cast<float>(inj[uz(i)]); }
        float*    host[3] = {hp.data(), hi.data(), ho.data()};
        const int lens[3] = {rc, rc, rc};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 3, static_cast<crd::u32>(rc / 64));
        int bad = 0;
        for (int i = 0; i < rc; ++i) { if (ho[uz(i)] != static_cast<float>(outo[uz(i)])) { ++bad; } }
        CHECK(bad == 0);
    }

    // ── (B) multi-cascade batched IFFT, batch = 4·C (C=3), bit-exact ──
    for (int batch : {4, 12, 16}) // a-2 single-cascade (4), a-3 three/four-cascade (12/16) — all bit-exact
    {
        kir::KGraph   g0(&alloc);
        kir::KGraph   g1(&alloc);
        kir::KGraph*  graphs[2] = {&g0, &g1};
        const kir::Fft2dPlan plan = kir::build_fft2d_c2c_batched(graphs, n, n, batch, true, 8);

        int off[16];
        int total = 0;
        for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
        crd::containers::Array<crd::f64> a64(&alloc);
        crd::containers::Array<float>    a32(&alloc);
        a64.resize(uz(total), 0.0);
        a32.resize(uz(total), 0.0F);
        crd::f64* h64[16];
        float*    h32[16];
        for (int b = 0; b < plan.nbuffers; ++b) { h64[b] = a64.data() + off[b]; h32[b] = a32.data() + off[b]; }

        constexpr crd::f64 two_pi = 6.28318530717958647693;
        const auto         f32d   = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
        for (int i = 0; i < rc * batch; ++i)
        {
            h64[plan.in_re][i] = static_cast<crd::f64>((i * 7 + 3) % 11 - 5);
            h64[plan.in_im][i] = static_cast<crd::f64>((i * 5 + 1) % 13 - 6);
        }
        for (int k = 0; k < n; ++k)
        {
            const crd::f64 a       = two_pi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
            h64[plan.tw_col_re][k] = f32d(crd::math::cos(a));
            h64[plan.tw_col_im][k] = f32d(-crd::math::sin(a));
            h64[plan.tw_row_re][k] = h64[plan.tw_col_re][k];
            h64[plan.tw_row_im][k] = h64[plan.tw_col_im][k];
        }
        for (int i = 0; i < total; ++i) { a32[uz(i)] = static_cast<float>(a64[uz(i)]); }
        crd::kir_test::run_fft2d_cpu(plan, h64, &alloc);

        std::unique_ptr<crd::gpu::ComputePipeline> pipe_store[8];
        crd::gpu::ComputePipeline*                 pipes[8] = {};
        for (int pi = 0; pi < plan.npasses; ++pi)
        {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[pi].graph, plan.passes[pi].entry, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "casc", &alloc);
            REQUIRE(spv.ok);
            pipe_store[pi] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), plan.passes[pi].nbind, 0U);
            REQUIRE(pipe_store[pi] != nullptr);
            pipes[pi] = pipe_store[pi].get();
        }
        crd::kir_test::dispatch_fft2d(compute, plan, pipes, h32);

        int badr = 0;
        int badi = 0;
        for (int i = 0; i < rc * batch; ++i)
        {
            if (h32[plan.res_re][i] != static_cast<float>(h64[plan.res_re][i])) { ++badr; }
            if (h32[plan.res_im][i] != static_cast<float>(h64[plan.res_im][i])) { ++badi; }
        }
        WARN("[ocean-cascade-vk] " << n << "x" << n << " batch=" << batch << " re-bad " << badr << " im-bad " << badi);
        CHECK(badr == 0);
        CHECK(badi == 0);
    }
}

// B16-a-2 CRUSH BENCH: the FFT-ocean's BATCHED inverse 2-D FFT (build_fft2d_c2c_batched — 2-pass transpose-on-write strided)
// GPU-timed via last_gpu_ms (kernel only, upload/readback excluded — apples-to-apples with cuFFT's cudaEvent), min-of-30.
// Sweeps (n, batch) to span the L2-resident -> DRAM-bound crossover (48 MB L2 on Ada ⇒ n=512, batch>=32 spills). Self-verifies
// per point: a DC-only spectrum (V at index 0 of each image) inverse-FFTs to a CONSTANT field V ⇒ a fast wrong kernel fails.
// Hidden ([.ocean-ifft-bench]); compare per_image_ms to cufft_2d_c2c_batched_bench.exe. Board: docs/bench/2026-07-15-*.
TEST_CASE("B16-a-2: CKIR batched inverse 2-D FFT (the FFT-ocean transform) GPU benchmark vs cuFFT batched", "[.ocean-ifft-bench]")
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
    constexpr int      tile_c = 8;

    const int pts[6][2] = {{256, 4}, {256, 16}, {256, 64}, {512, 4}, {512, 16}, {512, 64}};
    std::printf("# CKIR batched inverse 2-D FFT (2-pass strided), Vulkan last_gpu_ms min-of-30\n");
    std::printf("# %-6s %-6s %-14s %-16s\n", "N", "B", "batch_ms", "per_image_ms");
    for (int pi = 0; pi < 6; ++pi)
    {
        const int            n     = pts[pi][0];
        const int            batch = pts[pi][1];
        const int            rc    = n * n;
        const float          vdc   = 3.5F; // DC value ⇒ constant field vdc
        kir::KGraph          g0(&alloc);
        kir::KGraph          g1(&alloc);
        kir::KGraph*         graphs[2] = {&g0, &g1};
        const kir::Fft2dPlan plan      = kir::build_fft2d_c2c_batched(graphs, n, n, batch, true, tile_c);

        std::unique_ptr<cg::ComputePipeline> pipe_store[8];
        cg::ComputePipeline*                 pipes[8] = {};
        bool                                 ok       = true;
        for (int p = 0; p < plan.npasses; ++p)
        {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(*plan.passes[p].graph, plan.passes[p].entry, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "oifftb", &alloc);
            if (!spv.ok) { ok = false; break; }
            pipe_store[p] = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), plan.passes[p].nbind, 0U);
            if (pipe_store[p] == nullptr) { ok = false; break; }
            pipes[p] = pipe_store[p].get();
        }
        if (!ok) { WARN("[ocean-ifft-bench] N=" << n << " B=" << batch << " SKIPPED (compile/pipeline)"); continue; }

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
        up(plan.in_re, [&](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = (i % rc == 0) ? vdc : 0.0F; } }); // DC per image
        up(plan.in_im, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = 0.0F; } });
        up(plan.tw_col_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_col_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });
        up(plan.tw_row_re, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(plan.tw_row_im, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });

        auto record = [&]() {
            auto& rec = compute.begin();
            for (int p = 0; p < plan.npasses; ++p)
            {
                const kir::Fft2dPass& pp       = plan.passes[p];
                cg::ComputeBuffer*    binds[8] = {};
                for (int k = 0; k < pp.nbind; ++k) { binds[k] = dev[pp.bind[k]].get(); }
                rec.dispatch(*pipes[p], crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(pp.nbind)), nullptr, 0U, pp.num_workgroups, 1U, 1U);
                if (p + 1 < plan.npasses) { for (int b = 0; b < plan.nbuffers; ++b) { rec.barrier(*dev[b], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); } }
            }
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 30; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }

        // self-verify: DC-only spectrum ⇒ constant field vdc (unnormalised inverse). Check the first 64 outputs of image 0.
        auto rb = compute.create_buffer(64U * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        { auto& rec = compute.begin(); rec.barrier(*dev[plan.res_re], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc); rec.copy(*dev[plan.res_re], *rb, 0U, 0U, 64U * sizeof(float)); compute.submit_and_wait(); }
        const auto* o   = static_cast<const float*>(rb->map());
        int         bad = 0;
        for (int i = 0; i < 64; ++i) { if (o[i] < vdc - 1e-3F || o[i] > vdc + 1e-3F) { ++bad; } }
        rb->unmap();
        std::printf("%-6d %-6d %-14.4f %-16.5f  verify_bad=%d\n", n, batch, best, best / batch, bad);
        CHECK(best < 1e29);
        CHECK(bad == 0);
    }
}

// B16-a-2 FUSION CRUSH BENCH: the FUSED ocean update (build_ocean_evolve_rowfft -> strided column IFFT = 2 dispatches, evolve
// folded into the row IFFT's first global load) vs the UN-FUSED (evolve -> row IFFT -> strided column IFFT = 3 dispatches).
// The fusion eliminates the evolve dispatch AND the whole packed-spectrum global round-trip (evolve writes 8·N² floats the row
// IFFT reads straight back). This is the fewer-global-round-trips win a cuFFT-based ocean CANNOT match: cuFFT cannot fuse the
// wave physics into its transform, so it MUST run a separate evolve kernel writing the spectrum (exactly the round-trip we
// delete). GPU-timed via last_gpu_ms, min-of-30, self-verifying (fused res == un-fused res BIT-EXACT — also proves the fused
// kernel runs correctly on real hardware). Hidden ([.ocean-fused-bench]). Batch = 4 packed fields (one cascade).
TEST_CASE("B16-a-2: FUSED ocean update vs un-fused (the fusion crush) GPU benchmark", "[.ocean-fused-bench]")
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
    constexpr crd::f64 two_pi = 6.28318530717958647693;
    const float        tval   = 1.5F;

    const int ns[2] = {256, 1024};
    std::printf("# FUSED vs un-fused ocean update, Vulkan last_gpu_ms min-of-30, batch=4 (one cascade)\n");
    std::printf("# %-6s %-14s %-14s %-10s %-8s\n", "N", "unfused_ms", "fused_ms", "speedup", "verify");
    for (int nidx = 0; nidx < 2; ++nidx)
    {
        const int n      = ns[nidx];
        const int rc     = n * n;
        const int tile_c = (n >= 1024) ? 4 : 8; // radix-16 tiled column shared (2*tile_c*(n+1) f32) must fit 48 KB
        kir::ocean::OceanConfig oc;
        oc.n            = n;
        oc.patch_length = crd::units::Length64{250.0};

        kir::KGraph          gevo(&alloc);
        kir::KGraph          grow(&alloc);
        kir::KGraph          gcol(&alloc);
        kir::KGraph          gfus(&alloc);
        const kir::KEntry    eevo = kir::ocean::build_ocean_evolve(gevo, oc);
        const kir::Fft1dPlan prow = kir::build_fft1d_radix4(grow, n, true, true);               // row IFFT (radix-4, matches the fused kernel)
        const kir::Fft1dPlan pcol = kir::build_fft1d_radix16(gcol, n, true, true, tile_c, n);   // strided column IFFT
        const kir::KEntry    efus = kir::ocean::build_ocean_evolve_rowfft(gfus, oc);            // fused evolve+row IFFT

        const auto make = [&](kir::KGraph& gg, const kir::KEntry& e, int nb, const char* nm) -> std::unique_ptr<cg::ComputePipeline> {
            kir::GlslKernel kern(&alloc);
            REQUIRE(kir::emit_compute_kernel_glsl(gg, e, &alloc, kern));
            const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), nm, &alloc);
            if (!spv.ok) { return nullptr; }
            return compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), nb, 0U);
        };
        auto p_evo = make(gevo, eevo, 4, "oevo");
        auto p_row = make(grow, prow.entry, 6, "orow");
        auto p_col = make(gcol, pcol.entry, 6, "ocol");
        auto p_fus = make(gfus, efus, 6, "ofus");
        if (p_evo == nullptr || p_row == nullptr || p_col == nullptr || p_fus == nullptr)
        {
            WARN("[ocean-fused-bench] N=" << n << " SKIPPED (compile/pipeline — likely shared-mem limit)");
            continue;
        }

        // logical buffers: 0 h0, 1 params, 2 tw_re, 3 tw_im, 4 spec_re, 5 spec_im, 6 xr, 7 xi, 8 resu_re, 9 resu_im, 10 resf_re, 11 resf_im
        const int          sizes[12] = {rc * 4, 1, n, n, rc * 4, rc * 4, rc * 4, rc * 4, rc * 4, rc * 4, rc * 4, rc * 4};
        std::unique_ptr<cg::ComputeBuffer> dev[12];
        for (int b = 0; b < 12; ++b)
        {
            dev[b] = compute.create_buffer(static_cast<crd::u64>(sizes[b]) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        }
        const auto up = [&](int id, auto fill) {
            auto  stg = compute.create_buffer(static_cast<crd::u64>(sizes[id]) * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
            auto* p   = static_cast<float*>(stg->map());
            fill(p, sizes[id]);
            stg->unmap();
            auto& rec = compute.begin();
            rec.copy(*stg, *dev[id], 0U, 0U, static_cast<crd::u64>(sizes[id]) * sizeof(float));
            compute.submit_and_wait();
        };
        up(0, [](float* p, int c) { for (int i = 0; i < c; ++i) { p[i] = static_cast<float>((i * 13 + 5) % 17 - 8) * 0.01F; } }); // synthetic h0
        up(1, [&](float* p, int) { p[0] = tval; });
        up(2, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(crd::math::cos(two_pi * k / n)); } });
        up(3, [&](float* p, int c) { for (int k = 0; k < c; ++k) { p[k] = static_cast<float>(-crd::math::sin(two_pi * k / n)); } });

        const auto dsp = [&](cg::ComputePipeline& pipe, const int* ids, int nb, crd::u32 grid, cg::ComputeRecorder& rec) {
            cg::ComputeBuffer* binds[8] = {};
            for (int k = 0; k < nb; ++k) { binds[k] = dev[ids[k]].get(); }
            rec.dispatch(pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, static_cast<crd::usize>(nb)), nullptr, 0U, grid, 1U, 1U);
            for (int b = 0; b < 12; ++b) { rec.barrier(*dev[b], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead); }
        };
        const int    evo_b[4] = {0, 1, 4, 5};
        const int    row_b[6] = {4, 5, 2, 3, 6, 7};
        const int    colu_b[6] = {6, 7, 2, 3, 8, 9};
        const int    fus_b[6] = {0, 1, 2, 3, 6, 7};
        const int    colf_b[6] = {6, 7, 2, 3, 10, 11};
        const crd::u32 grid_map  = static_cast<crd::u32>(rc / 64);
        const crd::u32 grid_row  = static_cast<crd::u32>(n * 4);
        const crd::u32 grid_col  = static_cast<crd::u32>((n / tile_c) * 4);

        const auto rec_unfused = [&]() {
            auto& rec = compute.begin();
            dsp(*p_evo, evo_b, 4, grid_map, rec);   // evolve -> spec
            dsp(*p_row, row_b, 6, grid_row, rec);   // row IFFT spec -> x
            dsp(*p_col, colu_b, 6, grid_col, rec);  // strided column IFFT x -> resu
            compute.submit_and_wait();
        };
        const auto rec_fused = [&]() {
            auto& rec = compute.begin();
            dsp(*p_fus, fus_b, 6, grid_row, rec);   // fused evolve+row IFFT h0 -> x
            dsp(*p_col, colf_b, 6, grid_col, rec);  // strided column IFFT x -> resf
            compute.submit_and_wait();
        };

        for (int w = 0; w < 5; ++w) { rec_unfused(); rec_fused(); }
        double bu = 1e30;
        double bf = 1e30;
        for (int r = 0; r < 30; ++r) { rec_unfused(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < bu) { bu = ms; } }
        for (int r = 0; r < 30; ++r) { rec_fused(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < bf) { bf = ms; } }

        // self-verify: fused res == un-fused res, bit-exact (the fusion + FFT are exact; GPU transcendentals are identical on
        // both paths). Read back the full 4-field result and compare.
        auto rbu = compute.create_buffer(static_cast<crd::u64>(rc * 4) * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        auto rbf = compute.create_buffer(static_cast<crd::u64>(rc * 4) * sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*dev[8], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.barrier(*dev[10], cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*dev[8], *rbu, 0U, 0U, static_cast<crd::u64>(rc * 4) * sizeof(float));
            rec.copy(*dev[10], *rbf, 0U, 0U, static_cast<crd::u64>(rc * 4) * sizeof(float));
            compute.submit_and_wait();
        }
        const auto* ou  = static_cast<const float*>(rbu->map());
        const auto* of  = static_cast<const float*>(rbf->map());
        int         bad = 0;
        for (int i = 0; i < rc * 4; ++i) { if (ou[i] != of[i]) { ++bad; } }
        rbu->unmap();
        rbf->unmap();

        std::printf("%-6d %-14.4f %-14.4f %-10.3f %-8d\n", n, bu, bf, bu / bf, bad);
        CHECK(bu < 1e29);
        CHECK(bf < 1e29);
        CHECK(bad == 0);
    }
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

// AS-4 OP-GENERALITY (the auto-scheduler beyond Contract): the SAME enumerate → cost-rank → MEASURE → oracle-validate → pick-best
// loop that tunes the GEMM, applied to a device-wide REDUCTION. `enumerate_reduce_schedules` yields the valid (threads, per_thread)
// space; the cost model ranks it; we MEASURE the top-K on the real GPU (each build_reduce'd, emitted, dispatched, GPU-timed), a
// reduction of all-ones ORACLE-VALIDATES each (sum must == N), and the fastest wins — proving the winner beats the hand-tuned
// (256,8) default and reaches CUB's bandwidth. This is what makes AS a GENERAL kernel auto-scheduler, not a GEMM special case.
// Hidden ([.reduce-autotune]) — a measured board.
TEST_CASE("AS-4: the auto-scheduler TUNES the reduction on-device -- enumerate/measure/validate vs CUB", "[.reduce-autotune]")
{
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    namespace at  = crd::kir::autotune;
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

    const int    n         = 1 << 24;   // 64 MB ⇒ spills the 48 MB L2 ⇒ DRAM-bound (CUB ~602 GB/s here)
    const double cub_dram_gbps = 602.6; // CUB DeviceReduce gold on this GPU (bench/gpu-compute/cub_reduce_bench)

    // the input (all 1.0 ⇒ sum == N, exact in f32) — created ONCE, shared across every candidate schedule.
    auto d_in = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
    {
        auto  stg = compute.create_buffer(static_cast<crd::u64>(n) * sizeof(float), transfer_src, cg::ComputeMemory::CpuToGpu);
        auto* p   = static_cast<float*>(stg->map());
        for (int i = 0; i < n; ++i) { p[i] = 1.0F; }
        stg->unmap();
        auto& rec = compute.begin();
        rec.copy(*stg, *d_in, 0U, 0U, static_cast<crd::u64>(n) * sizeof(float));
        compute.submit_and_wait();
    }

    // measure ONE reduce schedule: build_reduce(threads, nblocks) → emit → compile → dispatch (min-of-25) → readback the scalar.
    // returns the best GPU ms, and writes the reduced sum to `sum_out`. ms = 1e30 if the schedule fails to build/emit/compile.
    auto measure = [&](int threads, int nblocks, float& sum_out) -> double {
        kir::KGraph  g0(&alloc);
        kir::KGraph  g1(&alloc);
        kir::KGraph* graphs[2] = {&g0, &g1};
        const kir::ReducePlan plan = kir::build_reduce(graphs, n, kir::KOp::Add, threads, nblocks);
        kir::GlslKernel kb(&alloc);
        kir::GlslKernel kf(&alloc);
        if (!kir::emit_compute_kernel_glsl(*plan.block_graph, plan.block, &alloc, kb)) { return 1e30; }
        if (!kir::emit_compute_kernel_glsl(*plan.final_graph, plan.final_pass, &alloc, kf)) { return 1e30; }
        const auto sb = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kb.source), "ra_b", &alloc);
        const auto sf = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kf.source), "ra_f", &alloc);
        if (!sb.ok || !sf.ok) { return 1e30; }
        auto pb = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sb.spirv.data(), sb.spirv.size()), 2, 0U);
        auto pf = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(sf.spirv.data(), sf.spirv.size()), 2, 0U);
        if (pb == nullptr || pf == nullptr) { return 1e30; }
        auto d_part = compute.create_buffer(static_cast<crd::u64>(nblocks) * sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto d_out  = compute.create_buffer(sizeof(float), storage | transfer_dst | transfer_src, cg::ComputeMemory::GpuOnly);
        auto record = [&]() {
            auto&              rec  = compute.begin();
            cg::ComputeBuffer* b0[2] = {d_in.get(), d_part.get()};
            rec.dispatch(*pb, crd::containers::ConstSpan<cg::ComputeBuffer*>(b0, 2), nullptr, 0U, static_cast<crd::u32>(nblocks), 1U, 1U);
            rec.barrier(*d_part, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::ShaderRead);
            cg::ComputeBuffer* b1[2] = {d_part.get(), d_out.get()};
            rec.dispatch(*pf, crd::containers::ConstSpan<cg::ComputeBuffer*>(b1, 2), nullptr, 0U, 1U, 1U, 1U);
            compute.submit_and_wait();
        };
        for (int w = 0; w < 5; ++w) { record(); }
        double best = 1e30;
        for (int r = 0; r < 25; ++r) { record(); const double ms = compute.last_gpu_ms(); if (ms > 0.0 && ms < best) { best = ms; } }
        auto rb = compute.create_buffer(sizeof(float), transfer_dst, cg::ComputeMemory::GpuToCpu);
        {
            auto& rec = compute.begin();
            rec.barrier(*d_out, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            rec.copy(*d_out, *rb, 0U, 0U, sizeof(float));
            compute.submit_and_wait();
        }
        sum_out = *static_cast<const float*>(rb->map());
        rb->unmap();
        return best;
    };

    // 1. ENUMERATE + cost-rank the reduce schedule space, then MEASURE the top-K on-device.
    const at::DeviceLimits lim;
    const at::DeviceSpec   spec;
    at::ReduceSchedule     space[64];
    const int              cnt = at::enumerate_reduce_schedules(n, lim, space, 64);
    REQUIRE(cnt > 0);
    int       topk[10];
    const int nk = at::rank_reduce_top_k_cost(space, cnt, n, spec, topk, 10);

    double best_ms = 1e30;
    int    best_threads = 0;
    int    best_pt      = 0;
    int    measured     = 0;
    for (int t = 0; t < nk; ++t)
    {
        const at::ReduceSchedule& s  = space[topk[t]];
        const int                 nb = at::reduce_nblocks(n, s);
        float                     sum = 0.0F;
        const double              ms = measure(s.threads, nb, sum);
        if (ms > 1e29) { continue; }
        CHECK(sum == static_cast<float>(n)); // ORACLE: every candidate must compute the correct reduction (else it can't win)
        ++measured;
        const double gbps = static_cast<double>(n) * sizeof(float) / (ms * 1.0e6);
        WARN("[reduce-autotune] cand threads=" << s.threads << " pt=" << s.per_thread << " nblocks=" << nb << " -> " << ms << " ms (" << gbps << " GB/s)");
        if (ms < best_ms) { best_ms = ms; best_threads = s.threads; best_pt = s.per_thread; }
    }
    REQUIRE(measured > 0);

    // 2. the hand-tuned (256,8) DEFAULT as the baseline the autotuner must match or beat.
    float        ht_sum = 0.0F;
    const double ht_ms  = measure(256, at::reduce_nblocks(n, at::ReduceSchedule{256, 8}), ht_sum);
    CHECK(ht_sum == static_cast<float>(n));

    const double best_gbps = static_cast<double>(n) * sizeof(float) / (best_ms * 1.0e6);
    const double ht_gbps   = static_cast<double>(n) * sizeof(float) / (ht_ms * 1.0e6);
    WARN("[reduce-autotune] N=" << n << " (DRAM-bound) measured " << measured << "/" << nk << " top-K; WINNER threads=" << best_threads
                                << " pt=" << best_pt << " -> " << best_ms << " ms (" << best_gbps << " GB/s, " << (100.0 * best_gbps / 672.0)
                                << "% of peak) | hand-tuned 256x8 " << ht_gbps << " GB/s | CUB " << cub_dram_gbps << " GB/s | autotuned/CUB = "
                                << (best_gbps / cub_dram_gbps) << "x");
    // the auto-scheduler's winner is at least as fast as the hand-tuned default (that default is IN the search space, so the tuned
    // pick can never be worse up to measurement noise) and reaches CUB-class DRAM bandwidth.
    CHECK(best_ms <= ht_ms * 1.05);
    CHECK(best_gbps > 0.80 * cub_dram_gbps); // at least parity-class with CUB at the DRAM wall
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

    constexpr int n   = 16384;
    constexpr int epb = 1024;
    // ── REN-38 llvmpipe campaign: the sort SHAPE is DEVICE-DERIVED (subgroup width + shared budget), not a
    // hardwired warp-32 assumption — llvmpipe is 8-wide with 32 KB shared, where the 256-thread 8-bit shape is
    // both wrong (warp math) and unbuildable (32 KB of seg[] alone). Correctness is shape-independent.
    const crd::u32 lanes = compute.subgroup_size();
    if (lanes == 0U) { SKIP("device reports no subgroup width - the warp-synchronous sort cannot be shaped"); }
    const kir::SortConfig scfg    = kir::pick_sort_config(lanes, compute.shared_memory_bytes(), epb, false);
    const int             threads = scfg.threads;
    const int             radix_bits = scfg.radix_bits;
    const int             nbins   = 1 << radix_bits;
    const int             npasses = scfg.passes;
    constexpr int         nblocks = n / epb;

    const int scan_threads = nblocks < threads ? nblocks : threads; // divides nblocks
    std::unique_ptr<cg::ComputePipeline> ph_s[8];
    std::unique_ptr<cg::ComputePipeline> ps_s[8];
    std::unique_ptr<cg::ComputePipeline> po1_s;
    std::unique_ptr<cg::ComputePipeline> po2_s;
    cg::ComputePipeline*                 ph[8] = {};
    cg::ComputePipeline*                 ps[8] = {};
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
    crd::containers::Array<kir::KGraph> ghg(&alloc);
    crd::containers::Array<kir::KGraph> gsg(&alloc);
    for (int p = 0; p < npasses; ++p) { ghg.emplace_back(&alloc); gsg.emplace_back(&alloc); }
    for (int p = 0; p < npasses; ++p)
    {
        const crd::usize up = static_cast<crd::usize>(p);
        ph_s[p] = mk(ghg[up], kir::build_sort_histogram(ghg[up], epb, threads, radix_bits, p * radix_bits, nblocks,
                                                        static_cast<int>(lanes)), 2, "sort_hist");
        ps_s[p] = mk(gsg[up], kir::build_sort_scatter(gsg[up], epb, threads, radix_bits, p * radix_bits, nblocks,
                                                      static_cast<int>(lanes)), 4, "sort_scat");
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
    for (int p = 0; p < npasses; ++p)
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
        ph_s[p] = mk(ghg[p], kir::build_sort_histogram(ghg[p], epb, threads, radix_bits, p * 8, nblocks, 32), 2, "srt_h");
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter(gsg[p], epb, threads, radix_bits, p * 8, nblocks, 32), 4, "srt_s");
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

    constexpr int threads = 64;
    // REN-38 llvmpipe campaign: the ORACLE groups by the DEVICE subgroup width (llvmpipe 8, NV 32) — the old
    // hardwired /32 made the reference wrong, not the GPU.
    const crd::u32 sg_lanes = compute.subgroup_size();
    if (sg_lanes == 0U) { SKIP("device reports no subgroup width"); }

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
        const int sgbase = (t / static_cast<int>(sg_lanes)) * static_cast<int>(sg_lanes); // DEVICE width, not 32
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
    auto ph  = mk(gh, kir::build_sort_histogram(gh, epb, threads, radix_bits, 0, nblocks, 32), 2, "kp_h");
    auto po1 = mk(go1, kir::build_sort_offset_local(go1, nblocks, radix_bits, scan_threads), 3, "kp_o1");
    auto po2 = mk(go2, kir::build_sort_gbase(go2, radix_bits), 2, "kp_gb");
    auto ps  = mk(gs, kir::build_sort_scatter(gs, epb, threads, radix_bits, 0, nblocks, 32), 4, "kp_s");
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
        ps_s[p] = mk(gsg[p], kir::build_sort_scatter_onesweep(gsg[p], epb, threads, radix_bits, p * 8, p, nblocks, 32), 4, "osw_s");
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

// D-007 D2 (ADR-0104): the OFFLINE SHADER COOK. Cook a CKIR compute kernel into a `.crdr` bundle (serialized IR + IR-reflection +
// per-backend blobs), then prove (1) the cooked SPIR-V is BYTE-IDENTICAL to the runtime compile, (2) the bundle round-trips through
// the CRDR container, (3) the COOKED bytecode (not a fresh compile) RUNS correctly on the GPU, and (4) the content-hash cache
// re-uses an unchanged cook byte-for-byte. This is the zero-runtime-compile shipping path.
TEST_CASE("D-007 D2: offline cook -- CKIR kernel to .crdr bundle; cooked SPIR-V bit-identical + runs on GPU",
          "[gpu-context][vulkan][gpu][cook]")
{
    namespace sc  = crd::shadercook;
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    // A reverse kernel: 2 F32 buffers in@(0,0) / out@(0,1), ls=32 (the shared B-cmp harness kernel).
    constexpr int ls = 32;
    kir::KGraph    g(&alloc);
    const kir::KEntry e = crd::kir_test::build_reverse_kernel(g, ls);

    // COOK every backend.
    sc::CookOptions opts;
    opts.backends = static_cast<crd::u32>(sc::CookBackend::All);
    sc::CookResult ck = sc::cook_compute_shader(g, e, crd::containers::StringView("reverse"), opts, &alloc);
    REQUIRE(ck.ok);
    CHECK(ck.spirv_bytes > 0U); // SPIR-V + DXIL are REAL bytecode
    // the documented dxc soft-skip (compile_hlsl_to_spirv's contract): DXIL cooks only where the SDK ships
    // dxc (CRD_HAS_DXC=0 on Linux without dxc-dev) — the availability PROBE gates the DXIL halves truthfully.
    const bool dxil_available = crd::gpu::compile_hlsl_to_spirv(gpu::ShaderStage::Compute,
                                                                crd::containers::StringView(
                                                                    "[numthreads(1,1,1)] void main() {}"),
                                                                crd::containers::StringView("dxc_probe"), &alloc)
                                    .ok;
    if (dxil_available) { CHECK(ck.dxil_bytes > 0U); }
    else { std::printf("[dxc unavailable] DXIL cook assertions soft-skipped (CRD_HAS_DXC=0)\n"); }
    CHECK(ck.cuda_bytes  > 0U); // CUDA/MSL/WGSL are emitted source (their platform toolchain finishes the compile)
    CHECK(ck.msl_bytes   > 0U);
    CHECK(ck.wgsl_bytes  > 0U);

    // (2) Round-trip through the CRDR container: every chunk is present.
    sc::ShaderBundle bundle(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(ck.crdr), bundle));
    const auto spvc = bundle.bytecode(sc::CookBackend::SpirV);
    REQUIRE(!spvc.empty());
    if (dxil_available) { CHECK(!bundle.bytecode(sc::CookBackend::Dxil).empty()); }
    CHECK(!bundle.ir().empty());
    CHECK(bundle.reflection().size() == sizeof(kir::ShaderReflection));

    // (1) BIT-IDENTICAL: the cooked SPVC blob == a fresh runtime GLSL->SPIR-V of the same graph, byte for byte.
    kir::GlslKernel kern(&alloc);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &alloc, kern));
    const auto ref = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "reverse", &alloc);
    REQUIRE(ref.ok);
    REQUIRE(spvc.size() == ref.spirv.size());
    CHECK(std::memcmp(spvc.data(), ref.spirv.data(), spvc.size()) == 0);

    // (3) RUN the COOKED SPIR-V (straight from the bundle — no fresh compile) and check it reverses.
    auto pipe = compute.create_pipeline_from_spirv(spvc, 2, 0U);
    REQUIRE(pipe != nullptr);
    float in_h[ls];
    float out_h[ls];
    for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 0.5F; out_h[i] = 0.0F; }
    float*    host[2] = {in_h, out_h};
    const int lens[2] = {ls, ls};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
    int mism = 0;
    for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[ls - 1 - i]) { ++mism; } }
    std::printf("[cook] .crdr cooked: spirv=%u dxil=%u ptx=%u cuda=%u msl=%u wgsl=%u B; cooked SPIR-V ran on GPU, %d/%d reversed\n",
                ck.spirv_bytes, ck.dxil_bytes, ck.ptx_bytes, ck.cuda_bytes, ck.msl_bytes, ck.wgsl_bytes, ls - mism, ls);
    CHECK(mism == 0);

    // REAL PTX (NVRTC) — cooked wherever the CUDA toolkit is present; the bundle carries it as the CUDA target's shipping bytecode.
    const auto ptx = bundle.ptx();
    if (ck.ptx_bytes > 0U)
    {
        REQUIRE(!ptx.empty());
        // NVRTC PTX is text beginning with a `//` banner and carrying a `.version` directive — a cheap sanity that it's real PTX.
        crd::containers::StringView ptx_sv(reinterpret_cast<const char*>(ptx.data()), ptx.size());
        CHECK(ptx.size() > 64U);
        CHECK(ptx_sv.find(".version") != crd::containers::StringView::npos);
        CHECK(ptx_sv.find(".target") != crd::containers::StringView::npos);
        std::printf("[cook] real PTX cooked via NVRTC: %u B (portable virtual-arch bytecode)\n", ck.ptx_bytes);
    }
    else { WARN("no CUDA toolkit in this build; CUDA emitted as source only (no PTX)"); }

    // (4) CONTENT-HASH CACHE: first cook writes, second cook re-uses the exact bytes.
    const char* cache_dir = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/cook-cache";
    (void)crd::platform::fs::create_directories(crd::platform::fs::Path(cache_dir));
    sc::CookOptions copts = opts;
    copts.cache_dir       = cache_dir;
    sc::CookResult c1 = sc::cook_compute_shader(g, e, crd::containers::StringView("reverse"), copts, &alloc);
    REQUIRE(c1.ok);
    sc::CookResult c2 = sc::cook_compute_shader(g, e, crd::containers::StringView("reverse"), copts, &alloc);
    REQUIRE(c2.ok);
    CHECK(c2.from_cache);
    REQUIRE(c1.crdr.size() == c2.crdr.size());
    CHECK(std::memcmp(c1.crdr.data(), c2.crdr.data(), c1.crdr.size()) == 0);

    // Emit the serialized IR as a `.kgph` so the standalone `shader_cook` CLI can cook it (drives the D2 CLI smoke).
    crd::containers::Array<crd::u8> kgph = kir::serialize_graph(g, e, &alloc);
    (void)crd::platform::fs::write_file_binary(crd::platform::fs::Path(
        "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/reverse.kgph"),
        crd::containers::as_const_span(kgph));
}

// D-007 D3: a variant builder — bit0 scales the output (×1 or ×2); higher bits are DEAD (ignored), so keys differing only in
// them build identical IR and DEDUP. A real material compiler emits the live path per key exactly like this.
namespace
{
crd::kir::KEntry build_scale_variant(crd::kir::KGraph& g, crd::u32 key, void* /*user*/)
{
    namespace k        = crd::kir;
    const int    inbuf  = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int    outbuf = g.buffer_decl(k::DType::F32, 0, 1, true);
    const int    lid    = g.builtin(k::KBuiltin::LocalInvocationIndex);
    const auto   sh1    = k::make_shape({1});
    const double scale  = (key & 1U) != 0U ? 2.0 : 1.0; // bit0 = scale; higher bits DEAD ⇒ they dedup
    const int    mark   = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid, g.binary(k::KOp::Mul, g.buffer_load(inbuf, lid), g.constant(scale, sh1, k::DType::F32)));
    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = 32;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

// D-007 D3 (ADR-0104): the VARIANT / PERMUTATION system. Cook a matrix of variant keys with CONTENT-HASH DEDUP (identical
// specialized kernels share one bundle), ON-DEMAND (only the requested keys), report the reduction, and prove each cooked
// variant runs GPU-correct.
TEST_CASE("D-007 D3: variant matrix -- content-hash dedup + on-demand cook + GPU-correct per variant",
          "[gpu-context][vulkan][gpu][variant]")
{
    namespace sc  = crd::shadercook;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    const char* cache_dir = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/variant-cache";
    (void)crd::platform::fs::create_directories(crd::platform::fs::Path(cache_dir));

    // 4 requested variants; bit0 = scale(1|2), higher bits DEAD ⇒ {0,2} identical, {1,3} identical ⇒ 2 unique cooks.
    const crd::u32  keys[4] = {0U, 1U, 2U, 3U};
    sc::CookOptions opts;
    opts.backends  = static_cast<crd::u32>(sc::CookBackend::SpirV);
    opts.cache_dir = cache_dir;
    sc::VariantMatrixResult vm = sc::cook_variant_matrix(&build_scale_variant, nullptr, keys, 4, opts, &alloc);
    REQUIRE(vm.ok);
    std::printf("[variant] requested=%u unique=%u (%.0f%% permutation reduction via content-hash dedup)\n", vm.requested,
                vm.unique, 100.0 * (1.0 - (static_cast<double>(vm.unique) / static_cast<double>(vm.requested))));
    CHECK(vm.requested == 4U);
    CHECK(vm.unique == 2U); // DEDUP: 4 keys collapse to 2 distinct cooked bundles
    REQUIRE(vm.entries.size() == 4U);
    CHECK(vm.entries[0].hash == vm.entries[2].hash);    // key0 == key2 (bit1 dead)
    CHECK(vm.entries[1].hash == vm.entries[3].hash);    // key1 == key3
    CHECK(!(vm.entries[0].hash == vm.entries[1].hash)); // scale1 != scale2

    // On-demand + GPU-correct: cook one variant, run its cooked SPIR-V, verify the scale.
    const auto run_variant = [&](crd::u32 key, float expect_scale) {
        sc::CookResult r = sc::cook_one_variant(&build_scale_variant, nullptr, key, crd::containers::StringView("v"), opts, &alloc);
        REQUIRE(r.ok);
        sc::ShaderBundle b(&alloc);
        REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(r.crdr), b));
        auto pipe = compute.create_pipeline_from_spirv(b.bytecode(sc::CookBackend::SpirV), 2, 0U);
        REQUIRE(pipe != nullptr);
        constexpr int ls = 32;
        float         in_h[ls];
        float         out_h[ls];
        for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 1.0F; out_h[i] = 0.0F; }
        float*    host[2] = {in_h, out_h};
        const int lens[2] = {ls, ls};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
        int bad = 0;
        for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[i] * expect_scale) { ++bad; } }
        CHECK(bad == 0);
    };
    run_variant(0U, 1.0F); // scale 1
    run_variant(1U, 2.0F); // scale 2
    run_variant(2U, 1.0F); // bit1 dead → identical to key0
    std::printf("[variant] on-demand variants ran GPU-correct (scale 1 and 2)\n");
}

// D-007 D4 (ADR-0104): RUNTIME LOAD + PERSISTENT PIPELINE CACHE — the payoff of the whole D1–D5 chain. (1) A cooked `.crdr`
// is written to disk, read back fresh, and turned into a dispatchable pipeline from the cooked SPIR-V + the IR-reflection —
// with ZERO shader compilation (no shaderc). (2) The driver's SPIR-V→ISA pipeline cache is serialized, validated, and used to
// warm a fresh context (a warm restart reuses the compile).
TEST_CASE("D-007 D4: zero-compile runtime load from .crdr + persistent VkPipelineCache warm restart",
          "[gpu-context][vulkan][gpu][d4]")
{
    namespace sc  = crd::shadercook;
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    // COOK a kernel, write the .crdr to disk.
    constexpr int     ls = 32;
    kir::KGraph       g(&alloc);
    const kir::KEntry e = crd::kir_test::build_reverse_kernel(g, ls);
    sc::CookOptions   opts;
    opts.backends     = static_cast<crd::u32>(sc::CookBackend::SpirV);
    sc::CookResult ck = sc::cook_compute_shader(g, e, crd::containers::StringView("reverse"), opts, &alloc);
    REQUIRE(ck.ok);
    const char* path = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/d4_reverse.crdr";
    REQUIRE(crd::platform::fs::write_file_binary(crd::platform::fs::Path(path), crd::containers::as_const_span(ck.crdr)));

    // LOAD it back FRESH (a shipped bundle) — parse → cooked SPIR-V + IR-reflection → pipeline, NO compiler.
    crd::containers::Array<crd::u8> loaded(&alloc);
    REQUIRE(crd::platform::fs::read_file_binary(crd::platform::fs::Path(path), loaded));
    sc::ShaderBundle b(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(loaded), b));
    const auto refl_span = b.reflection();
    REQUIRE(refl_span.size() == sizeof(kir::ShaderReflection));
    kir::ShaderReflection refl{};
    std::memcpy(&refl, refl_span.data(), sizeof(refl));
    REQUIRE(refl.n_bindings == 2); // the reverse kernel: input + output
    auto pipe = compute.create_pipeline_from_spirv(b.bytecode(sc::CookBackend::SpirV), refl.n_bindings, 0U);
    REQUIRE(pipe != nullptr);

    float in_h[ls];
    float out_h[ls];
    for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 0.5F; out_h[i] = 0.0F; }
    float*    host[2] = {in_h, out_h};
    const int lens[2] = {ls, ls};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
    int mism = 0;
    for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[ls - 1 - i]) { ++mism; } }
    CHECK(mism == 0);
    std::printf("[d4] loaded cooked .crdr (%zu B) from disk with ZERO compile (reflection n_bindings=%d); ran %d/%d reversed\n",
                static_cast<size_t>(loaded.size()), refl.n_bindings, ls - mism, ls);

    // PERSISTENT PIPELINE CACHE: creating the pipeline grew the cache; serialize + validate the VkPipelineCache header.
    crd::containers::Array<crd::u8> cache_blob(&alloc);
    compute.pipeline_cache_data(cache_blob);
    REQUIRE(cache_blob.size() >= 32U); // the VkPipelineCache header is >= 32 bytes
    crd::u32 hdr_ver = 0;
    std::memcpy(&hdr_ver, cache_blob.data() + 4, 4); // [4..7] = VkPipelineCacheHeaderVersion
    CHECK(hdr_ver == 1U);              // VK_PIPELINE_CACHE_HEADER_VERSION_ONE

    // WARM RESTART: a FRESH context seeded with the persisted blob creates the same pipeline reusing the cached ISA.
    cg::VulkanComputeContext compute2(*vk, crd::memory::default_allocator());
    REQUIRE(compute2.valid());
    REQUIRE(compute2.warm_pipeline_cache(crd::containers::as_const_span(cache_blob)));
    auto pipe2 = compute2.create_pipeline_from_spirv(b.bytecode(sc::CookBackend::SpirV), refl.n_bindings, 0U);
    REQUIRE(pipe2 != nullptr);
    crd::containers::Array<crd::u8> cache_blob2(&alloc);
    compute2.pipeline_cache_data(cache_blob2);
    CHECK(cache_blob2.size() >= 32U); // the warmed cache still holds our pipeline
    std::printf("[d4] VkPipelineCache persisted %zu B (header v%u); a fresh context warm-started from it\n",
                static_cast<size_t>(cache_blob.size()), hdr_ver);
}

// D-007 D5: the pipeline create-callback for the hot-reloader — build a Vulkan pipeline from cooked SPIR-V (`user` = the context).
namespace
{
std::unique_ptr<crd::gpu::ComputePipeline> vk_create_from_spirv(crd::containers::ConstSpan<crd::u8> code, int n_bindings, void* user)
{
    return static_cast<crd::gpu::VulkanComputeContext*>(user)->create_pipeline_from_spirv(code, n_bindings, 0U);
}
} // namespace

// D-007 D5 (ADR-0104): HOT-RELOAD — the last link. Edit the shader graph, recook, and atomically hot-swap the live pipeline in
// the SAME context with no restart. An unchanged graph is a cheap no-op (content-hash gated); a changed one swaps and bumps the
// generation. Proven by running the SAME pipeline slot before and after an edit and watching the GPU behaviour change.
TEST_CASE("D-007 D5: hot-reload -- edit the IR, atomically swap the live pipeline in place", "[gpu-context][vulkan][gpu][d5]")
{
    namespace sc  = crd::shadercook;
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    sc::ReloadableCompute rc(&alloc);
    constexpr int         ls = 32;
    const auto            run = [&](float expect_scale) {
        REQUIRE(rc.pipeline() != nullptr);
        float in_h[ls];
        float out_h[ls];
        for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 1.0F; out_h[i] = 0.0F; }
        float*    host[2] = {in_h, out_h};
        const int lens[2] = {ls, ls};
        crd::kir_test::dispatch_kernel_1wg(compute, *rc.pipeline(), host, lens, 2, 1U);
        int bad = 0;
        for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[i] * expect_scale) { ++bad; } }
        return bad;
    };

    // v1: the kernel scales by ×1. First load ⇒ a swap, generation 1.
    kir::KGraph       g1(&alloc);
    const kir::KEntry e1 = build_scale_variant(g1, 0U, nullptr);
    sc::ReloadableCompute::Status s1 = rc.reload(g1, e1, crd::containers::StringView("k"), sc::CookBackend::SpirV, &vk_create_from_spirv, &compute);
    REQUIRE(s1.ok);
    CHECK(s1.changed);
    CHECK(rc.generation() == 1U);
    CHECK(run(1.0F) == 0);

    // Re-cook the SAME graph ⇒ the content hash is unchanged ⇒ a NO-OP (no rebuild, generation stays).
    kir::KGraph       g1b(&alloc);
    const kir::KEntry e1b = build_scale_variant(g1b, 0U, nullptr);
    sc::ReloadableCompute::Status s2 = rc.reload(g1b, e1b, crd::containers::StringView("k"), sc::CookBackend::SpirV, &vk_create_from_spirv, &compute);
    REQUIRE(s2.ok);
    CHECK_FALSE(s2.changed);
    CHECK(rc.generation() == 1U);

    // EDIT the shader: the kernel now scales by ×2 ⇒ hot-swap the live pipeline (generation 2), same context, no restart.
    kir::KGraph       g2(&alloc);
    const kir::KEntry e2 = build_scale_variant(g2, 1U, nullptr);
    sc::ReloadableCompute::Status s3 = rc.reload(g2, e2, crd::containers::StringView("k"), sc::CookBackend::SpirV, &vk_create_from_spirv, &compute);
    REQUIRE(s3.ok);
    CHECK(s3.changed);
    CHECK(rc.generation() == 2U);
    CHECK(run(2.0F) == 0); // the SAME slot now runs the EDITED kernel
    std::printf("[d5] hot-reload: gen1 (x1) -> unchanged re-cook is a no-op -> gen2 (x2); the live pipeline swapped in place\n");
}

// D-007 D3 (ÜBERGRAPH STYLE): the SAME 4 variants as [variant], but authored declaratively — ONE übergraph with ShaderOption
// selector nodes + an option-gated Select, specialized per key by sc::specialize (pin options to the key's bits + const-fold/DCE).
// bit0 picks the ×2 branch; opt1 is declared but UNUSED (dead) so keys differing only in it fold to identical IR and dedup.
namespace
{
crd::kir::KEntry build_scale_ubergraph(crd::kir::KGraph& g, crd::u32 key, void* /*user*/)
{
    namespace k        = crd::kir;
    const auto sh1     = k::make_shape({1});
    const int  inbuf   = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int  outbuf  = g.buffer_decl(k::DType::F32, 0, 1, true);
    const int  lid     = g.builtin(k::KBuiltin::LocalInvocationIndex);
    const int  opt0    = g.constant(0.0, sh1, k::DType::F32); // ShaderOption 0 — selects the scale (pinned per key)
    const int  opt1    = g.constant(0.0, sh1, k::DType::F32); // ShaderOption 1 — declared but UNUSED (dead) ⇒ dedups
    const int  cond    = g.binary(k::KOp::CmpGt, opt0, g.constant(0.5, sh1, k::DType::F32));
    const int  scale   = g.select(cond, g.constant(2.0, sh1, k::DType::F32), g.constant(1.0, sh1, k::DType::F32));
    const int  mark    = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid, g.binary(k::KOp::Mul, g.buffer_load(inbuf, lid), scale));
    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = 32;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    const int options[2] = {opt0, opt1};
    crd::shadercook::specialize(g, e, options, key, 2); // ← übergraph → specialized kernel, same cook path as the per-key style
    return e;
}
} // namespace

TEST_CASE("D-007 D3: ubergraph variant style -- pin ShaderOptions + specialize, same cook_variant_matrix, same dedup",
          "[gpu-context][vulkan][gpu][variant][ubergraph]")
{
    namespace sc = crd::shadercook;
    namespace cg = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    const char* cache_dir = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/uber-cache";
    (void)crd::platform::fs::create_directories(crd::platform::fs::Path(cache_dir));

    // The übergraph flows through the SAME cook_variant_matrix as the per-key builder — the only difference is the builder body.
    const crd::u32  keys[4] = {0U, 1U, 2U, 3U};
    sc::CookOptions opts;
    opts.backends  = static_cast<crd::u32>(sc::CookBackend::SpirV);
    opts.cache_dir = cache_dir;
    sc::VariantMatrixResult vm = sc::cook_variant_matrix(&build_scale_ubergraph, nullptr, keys, 4, opts, &alloc);
    REQUIRE(vm.ok);
    std::printf("[ubergraph] pin-option + specialize: requested=%u unique=%u (same %.0f%% dedup as the per-key style)\n",
                vm.requested, vm.unique, 100.0 * (1.0 - (static_cast<double>(vm.unique) / static_cast<double>(vm.requested))));
    CHECK(vm.requested == 4U);
    CHECK(vm.unique == 2U);                          // DCE folded opt1 away ⇒ 4 keys collapse to 2 (identical to the per-key style)
    REQUIRE(vm.entries.size() == 4U);
    CHECK(vm.entries[0].hash == vm.entries[2].hash); // opt1 dead ⇒ key0 == key2
    CHECK(vm.entries[1].hash == vm.entries[3].hash); // key1 == key3
    CHECK(!(vm.entries[0].hash == vm.entries[1].hash));

    // Each specialized übergraph runs GPU-correct — the pinned+DCE'd kernel scales by ×1 / ×2.
    const auto run_uber = [&](crd::u32 key, float expect_scale) {
        sc::CookResult r = sc::cook_one_variant(&build_scale_ubergraph, nullptr, key, crd::containers::StringView("u"), opts, &alloc);
        REQUIRE(r.ok);
        sc::ShaderBundle b(&alloc);
        REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(r.crdr), b));
        auto pipe = compute.create_pipeline_from_spirv(b.bytecode(sc::CookBackend::SpirV), 2, 0U);
        REQUIRE(pipe != nullptr);
        constexpr int ls = 32;
        float         in_h[ls];
        float         out_h[ls];
        for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 1.0F; out_h[i] = 0.0F; }
        float*    host[2] = {in_h, out_h};
        const int lens[2] = {ls, ls};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
        int bad = 0;
        for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[i] * expect_scale) { ++bad; } }
        CHECK(bad == 0);
    };
    run_uber(0U, 1.0F); // opt0=0 → Select folds to ×1
    run_uber(1U, 2.0F); // opt0=1 → Select folds to ×2
    std::printf("[ubergraph] specialized kernels ran GPU-correct (Select DCE'd to x1 / x2)\n");
}

// D-007 D3 (MATERIALS): a REAL OpenPBR material — a Fragment graph that outputs the B5 surface slab and does NO lighting —
// authored with feature toggles as ShaderOptions, specialized per variant via the material seam (cook::specialize_variant), and
// cooked through the SAME cook_variant_matrix as a compute kernel (the cook is now stage-aware). Proves materials dedup + cook
// to real fragment SPIR-V. bit0 = emissive on/off (REAL); bit1 = a 'debug' option declared but UNUSED (dead) ⇒ dedups.
namespace
{
crd::kir::KEntry build_material_variant(crd::kir::KGraph& g, crd::u32 key, void* /*user*/)
{
    namespace k   = crd::kir;
    namespace mat = crd::kir::material;
    namespace ck  = crd::kir::cook;
    const auto sh1 = k::make_shape({1});
    const auto kf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };

    // per-fragment interpolated varyings (a real VS supplies these; here they're stage inputs)
    ck::SurfaceInputs in;
    in.uv           = g.stage_in(k::KType::vec(k::DType::F32, 2), 0, k::Interp::Smooth);
    in.world_normal = g.stage_in(k::KType::vec(k::DType::F32, 3), 1, k::Interp::Smooth);

    // two ShaderOption feature toggles (pinned per variant by specialize_variant)
    const int opt_emissive = g.constant(0.0, sh1, k::DType::F32); // feature 0 — emissive on/off (REAL, changes the surface)
    const int opt_debug    = g.constant(0.0, sh1, k::DType::F32); // feature 1 — declared but UNUSED (dead) ⇒ keys dedup

    // surface fields (the OpenPBR slab)
    const int base     = g.vec3(g.swizzle(in.uv, 0), g.swizzle(in.uv, 1), kf(0.5)); // base color from the uv
    const int normal   = g.normalize(in.world_normal);
    const int emis_on  = g.vec3(kf(2.0), kf(1.5), kf(0.5));
    const int black    = g.vec3(kf(0.0), kf(0.0), kf(0.0));
    const int emissive = g.select(g.binary(k::KOp::CmpGt, opt_emissive, kf(0.5)), emis_on, black); // EMISSIVE toggle

    const int sid  = mat::define_surface(g);
    const int surf = mat::build_surface(g, sid, base, kf(0.0), kf(0.5), normal, emissive, kf(1.0), kf(1.0));
    k::KEntry  e;
    mat::pack_gbuffer(g, e, surf); // → KStage::Fragment, n_out=4 (the deferred G-buffer)

    // specialize to the key — the SAME unified helper as a compute kernel; it dispatches on the entry stage (here material:
    // pin ALL options, then lower_entry folds the option-gated Selects + DCEs the dead branches / unused options).
    const int opts[2] = {opt_emissive, opt_debug};
    crd::shadercook::specialize(g, e, opts, key, 2);
    return e;
}
} // namespace

TEST_CASE("D-007 D3: a REAL OpenPBR material through the variant matrix -- fragment cook + dedup",
          "[gpu-context][vulkan][gpu][variant][material]")
{
    namespace sc = crd::shadercook;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    const char* cache_dir = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/material-cache";
    (void)crd::platform::fs::create_directories(crd::platform::fs::Path(cache_dir));

    // The SAME cook_variant_matrix as compute kernels — the builder just returns a FRAGMENT (material) entry, and the now
    // stage-aware cook emits it through emit_stage_glsl → real fragment SPIR-V.
    const crd::u32  keys[4] = {0U, 1U, 2U, 3U};
    sc::CookOptions opts;
    opts.backends  = static_cast<crd::u32>(sc::CookBackend::SpirV);
    opts.cache_dir = cache_dir;
    sc::VariantMatrixResult vm = sc::cook_variant_matrix(&build_material_variant, nullptr, keys, 4, opts, &alloc);
    REQUIRE(vm.ok);
    std::printf("[material] OpenPBR surface variants: requested=%u unique=%u (%.0f%% dedup) — a real Fragment/G-buffer shader\n",
                vm.requested, vm.unique, 100.0 * (1.0 - (static_cast<double>(vm.unique) / static_cast<double>(vm.requested))));
    CHECK(vm.requested == 4U);
    CHECK(vm.unique == 2U); // the dead 'debug' option DCE'd ⇒ 4 material variants collapse to 2 bundles
    REQUIRE(vm.entries.size() == 4U);
    CHECK(vm.entries[0].hash == vm.entries[2].hash);    // emissive-off variants (key0, key2) share a bundle
    CHECK(vm.entries[1].hash == vm.entries[3].hash);    // emissive-on variants (key1, key3) share a bundle
    CHECK(!(vm.entries[0].hash == vm.entries[1].hash)); // emissive off ≠ on

    // The cooked artifact is REAL fragment SPIR-V — check the magic word and load it into a VkShaderModule (the driver validates).
    sc::CookOptions vopts;
    vopts.backends = static_cast<crd::u32>(sc::CookBackend::SpirV); // fresh cook (no cache) — the emissive-on variant
    sc::CookResult r = sc::cook_one_variant(&build_material_variant, nullptr, 1U, crd::containers::StringView("mat"), vopts, &alloc);
    REQUIRE(r.ok);
    CHECK(r.spirv_bytes > 0U);
    sc::ShaderBundle b(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(r.crdr), b));
    const auto spv = b.bytecode(sc::CookBackend::SpirV);
    REQUIRE(spv.size() >= 4U);
    crd::u32 magic = 0;
    std::memcpy(&magic, spv.data(), 4);
    CHECK(magic == 0x07230203U); // SPIR-V magic
    VkShaderModuleCreateInfo smci{};
    smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = spv.size();
    smci.pCode    = reinterpret_cast<const crd::u32*>(spv.data());
    VkShaderModule sm = VK_NULL_HANDLE;
    const VkResult vr = vkCreateShaderModule(vk->vk_device(), &smci, nullptr, &sm);
    CHECK(vr == VK_SUCCESS);
    if (sm != VK_NULL_HANDLE) { vkDestroyShaderModule(vk->vk_device(), sm, nullptr); }
    std::printf("[material] cooked Fragment SPIR-V (%zu B) accepted by the driver as a VkShaderModule\n", static_cast<size_t>(spv.size()));
}

// D-007 D2 (RASTER BUNDLE): a material is only a complete shippable PROGRAM as a vertex+fragment PAIR. Build a VS (transforms
// the vertex + passes the varyings the material reads) and the material FS in ONE graph, cook them into a single .crdr with
// SPVV (vertex) + SPVF (fragment) real bytecode, and verify BOTH stages create VkShaderModules — a complete graphics program.
namespace
{
crd::kir::KEntry build_test_vs(crd::kir::KGraph& g)
{
    namespace k        = crd::kir;
    const auto sh1     = k::make_shape({1});
    const int  pos     = g.stage_in(k::KType::vec(k::DType::F32, 3), 0, k::Interp::Smooth); // position attribute @ 0
    const int  uv      = g.stage_in(k::KType::vec(k::DType::F32, 2), 1, k::Interp::Smooth); // uv attribute @ 1
    const int  nrm     = g.stage_in(k::KType::vec(k::DType::F32, 3), 2, k::Interp::Smooth); // normal attribute @ 2
    k::KEntry  e;
    e.stage    = k::KStage::Vertex;
    e.position = g.vec4(g.swizzle(pos, 0), g.swizzle(pos, 1), g.swizzle(pos, 2), g.constant(1.0, sh1, k::DType::F32));
    e.n_out    = 2;
    e.out[0]   = {uv, 0};  // varying uv     → the FS reads it at location 0
    e.out[1]   = {nrm, 1}; // varying normal → the FS reads it at location 1
    return e;
}
crd::kir::KEntry build_material_fs(crd::kir::KGraph& g) // a fixed OpenPBR material (no variant toggles — VS+FS share the graph)
{
    namespace k   = crd::kir;
    namespace mat = crd::kir::material;
    namespace ck  = crd::kir::cook;
    const auto sh1 = k::make_shape({1});
    const auto kf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    ck::SurfaceInputs in;
    in.uv           = g.stage_in(k::KType::vec(k::DType::F32, 2), 0, k::Interp::Smooth);
    in.world_normal = g.stage_in(k::KType::vec(k::DType::F32, 3), 1, k::Interp::Smooth);
    const int base     = g.vec3(g.swizzle(in.uv, 0), g.swizzle(in.uv, 1), kf(0.5));
    const int normal   = g.normalize(in.world_normal);
    const int emissive = g.vec3(kf(0.2), kf(0.1), kf(0.0));
    const int sid      = mat::define_surface(g);
    const int surf     = mat::build_surface(g, sid, base, kf(0.0), kf(0.5), normal, emissive, kf(1.0), kf(1.0));
    k::KEntry e;
    mat::pack_gbuffer(g, e, surf);
    return e;
}
} // namespace

TEST_CASE("D-007 D2: cook a VS+FS raster program (material) into ONE .crdr bundle", "[gpu-context][vulkan][gpu][raster]")
{
    namespace sc = crd::shadercook;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    // VS + FS in ONE graph (the material fragment reads the VS varyings), cooked into one bundle.
    crd::kir::KGraph       g(&alloc);
    const crd::kir::KEntry vs = build_test_vs(g);
    const crd::kir::KEntry fs = build_material_fs(g);
    sc::CookOptions        opts;
    opts.backends     = static_cast<crd::u32>(sc::CookBackend::SpirV);
    sc::CookResult ck = sc::cook_raster_shader(g, vs, fs, crd::containers::StringView("mat_program"), opts, &alloc);
    REQUIRE(ck.ok);
    CHECK(ck.spirv_bytes > 0U);

    sc::ShaderBundle b(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(ck.crdr), b));
    const auto vspv = b.vertex_spirv();
    const auto fspv = b.fragment_spirv();
    REQUIRE(!vspv.empty());
    REQUIRE(!fspv.empty());

    // Both stages must be valid SPIR-V the driver accepts — that's a complete, loadable graphics program.
    const auto make_module = [&](crd::containers::ConstSpan<crd::u8> spv) {
        VkShaderModuleCreateInfo smci{};
        smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spv.size();
        smci.pCode    = reinterpret_cast<const crd::u32*>(spv.data());
        VkShaderModule sm = VK_NULL_HANDLE;
        const VkResult vr = vkCreateShaderModule(vk->vk_device(), &smci, nullptr, &sm);
        if (sm != VK_NULL_HANDLE) { vkDestroyShaderModule(vk->vk_device(), sm, nullptr); }
        return vr;
    };
    CHECK(make_module(vspv) == VK_SUCCESS);
    CHECK(make_module(fspv) == VK_SUCCESS);
    std::printf("[raster] cooked ONE .crdr with VS SPVV (%zu B) + FS SPVF (%zu B); both accepted as VkShaderModules — a full program\n",
                static_cast<size_t>(vspv.size()), static_cast<size_t>(fspv.size()));
}

// D-007 (neural → OpenPBR): a NEURAL material as a FRAGMENT shader that maps its coopvec-MLP outputs onto the OpenPBR surface
// slab and writes the deferred G-buffer — so a neural material feeds the SAME lighting pass as a conventional one (closing the
// gap that the neural render kernel wrote raw RGBA8). Proves coopvec runs per-pixel in the fragment stage and the G-buffer FS
// compiles to real SPIR-V the driver accepts.
TEST_CASE("D-007: neural material FRAGMENT writes the OpenPBR G-buffer (MLP outputs -> surface slab)",
          "[gpu-context][vulkan][gpu][neural][material]")
{
    namespace nn = crd::kir::neural;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->cooperative_vector()) { WARN("no VK_NV_cooperative_vector; skipping"); return; }
    crd::memory::TlsfAllocator alloc(16U << 20U);

    // A small MLP (in=hidden=out=16). Outputs [0..2]=base, [3]=metallic, [4]=roughness → the surface slab.
    nn::CoopVecMlpConfig  mcfg;
    crd::kir::GlslKernel  kern(&alloc);
    REQUIRE(nn::emit_neural_surface_fs_glsl(mcfg, kern));

    const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Fragment, crd::containers::to_view(kern.source), "neural_surface", &alloc);
    if (!spv.ok) { WARN("neural surface FS -> SPIR-V failed: " << spv.error_message.c_str()); }
    REQUIRE(spv.ok); // shaderc compiled a coopvec MLP in the FRAGMENT stage
    CHECK(!spv.spirv.empty());

    // If the device runs coopvec in the fragment stage, the G-buffer FS is a real, driver-accepted shader module.
    if ((vk->coopvec_supported_stages() & static_cast<crd::u32>(VK_SHADER_STAGE_FRAGMENT_BIT)) != 0U)
    {
        VkShaderModuleCreateInfo smci{};
        smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spv.spirv.size();
        smci.pCode    = reinterpret_cast<const crd::u32*>(spv.spirv.data());
        VkShaderModule sm = VK_NULL_HANDLE;
        const VkResult vr = vkCreateShaderModule(vk->vk_device(), &smci, nullptr, &sm);
        CHECK(vr == VK_SUCCESS);
        if (sm != VK_NULL_HANDLE) { vkDestroyShaderModule(vk->vk_device(), sm, nullptr); }
        std::printf("[neural] neural material FRAGMENT (coopvec MLP -> OpenPBR G-buffer, 4 MRT): %zu B SPIR-V, driver-accepted\n",
                    static_cast<size_t>(spv.spirv.size()));
    }
    else
    {
        std::printf("[neural] neural surface FS compiled to %zu B SPIR-V (coopvec fragment stage not device-enabled here)\n",
                    static_cast<size_t>(spv.spirv.size()));
    }
}

// D-007 D6: JOINT VS+FS variant specialization. A material übershader (feature toggles as ShaderOptions in the FS) cooked as a
// vertex+fragment PAIR sharing one graph, specialized per key as ONE unit (both entries' roots folded together — pinning one
// then the other would stale the sibling's node ids). bit0 = emissive on/off (real); bit1 = dead ⇒ dedups.
namespace
{
void build_material_program(crd::kir::KGraph& g, crd::u32 key, crd::kir::KEntry& vs_out, crd::kir::KEntry& fs_out)
{
    namespace k   = crd::kir;
    namespace mat = crd::kir::material;
    namespace ck  = crd::kir::cook;
    const auto sh1 = k::make_shape({1});
    const auto kf  = [&](double v) { return g.constant(v, sh1, k::DType::F32); };
    vs_out = build_test_vs(g); // the transform + varyings
    ck::SurfaceInputs in;
    in.uv           = g.stage_in(k::KType::vec(k::DType::F32, 2), 0, k::Interp::Smooth);
    in.world_normal = g.stage_in(k::KType::vec(k::DType::F32, 3), 1, k::Interp::Smooth);
    const int opt_emissive = g.constant(0.0, sh1, k::DType::F32); // feature 0
    const int opt_debug    = g.constant(0.0, sh1, k::DType::F32); // feature 1 — dead
    const int base     = g.vec3(g.swizzle(in.uv, 0), g.swizzle(in.uv, 1), kf(0.5));
    const int normal   = g.normalize(in.world_normal);
    const int emissive = g.select(g.binary(k::KOp::CmpGt, opt_emissive, kf(0.5)), g.vec3(kf(1.0), kf(0.5), kf(0.0)), g.vec3(kf(0.0), kf(0.0), kf(0.0)));
    const int sid      = mat::define_surface(g);
    const int surf     = mat::build_surface(g, sid, base, kf(0.0), kf(0.5), normal, emissive, kf(1.0), kf(1.0));
    mat::pack_gbuffer(g, fs_out, surf);
    const int opts[2] = {opt_emissive, opt_debug};
    crd::shadercook::specialize(g, vs_out, fs_out, opts, key, 2); // ← joint VS+FS specialization
}
} // namespace

TEST_CASE("D-007 D6: joint VS+FS variant specialization -- material ubershader cooks as VS+FS variants + dedups",
          "[gpu-context][vulkan][gpu][raster][d6]")
{
    namespace sc = crd::shadercook;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::memory::TlsfAllocator alloc(48U << 20U);

    const auto cook_key = [&](crd::u32 key) {
        crd::kir::KGraph g(&alloc);
        crd::kir::KEntry vs;
        crd::kir::KEntry fs;
        build_material_program(g, key, vs, fs);
        sc::CookOptions opts;
        opts.backends = static_cast<crd::u32>(sc::CookBackend::SpirV);
        return sc::cook_raster_shader(g, vs, fs, crd::containers::StringView("mat"), opts, &alloc);
    };
    const auto valid_module = [&](crd::containers::ConstSpan<crd::u8> spv) {
        VkShaderModuleCreateInfo smci{};
        smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spv.size();
        smci.pCode    = reinterpret_cast<const crd::u32*>(spv.data());
        VkShaderModule sm = VK_NULL_HANDLE;
        const VkResult vr = vkCreateShaderModule(vk->vk_device(), &smci, nullptr, &sm);
        if (sm != VK_NULL_HANDLE) { vkDestroyShaderModule(vk->vk_device(), sm, nullptr); }
        return vr == VK_SUCCESS;
    };

    // key 1 (emissive ON): a complete VS+FS raster material variant, both stages GPU-valid.
    sc::CookResult r1 = cook_key(1U);
    REQUIRE(r1.ok);
    CHECK(r1.spirv_bytes > 0U);
    sc::ShaderBundle b(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(r1.crdr), b));
    REQUIRE(!b.vertex_spirv().empty());
    REQUIRE(!b.fragment_spirv().empty());
    CHECK(valid_module(b.vertex_spirv()));
    CHECK(valid_module(b.fragment_spirv()));

    // DEDUP: keys 0 and 2 differ only in the DEAD bit1 ⇒ the joint specialize folds it away in BOTH stages ⇒ byte-identical bundles.
    sc::CookResult r0 = cook_key(0U);
    sc::CookResult r2 = cook_key(2U);
    REQUIRE(r0.ok);
    REQUIRE(r2.ok);
    REQUIRE(r0.crdr.size() == r2.crdr.size());
    CHECK(std::memcmp(r0.crdr.data(), r2.crdr.data(), r0.crdr.size()) == 0);
    // key 0 (emissive off) ≠ key 1 (emissive on).
    const bool same01 = (r0.crdr.size() == r1.crdr.size()) && std::memcmp(r0.crdr.data(), r1.crdr.data(), r0.crdr.size()) == 0;
    CHECK(!same01);
    std::printf("[d6] joint VS+FS specialize: key0==key2 (dead bit1, %zu B, deduped), key0!=key1 (emissive); both stages GPU-valid\n",
                static_cast<size_t>(r0.crdr.size()));
}

// D-007 D7: FULL RASTER REFLECTION. The raster bundle carries the complete graphics interface derived from the IR (no
// SPIRV-Cross): the VERTEX input layout (REFV — attributes) + the fragment descriptor bindings (REFL). So D4 builds a graphics
// PSO from the bundle alone. The test VS declares 3 attributes (position vec3 @0, uv vec2 @1, normal vec3 @2).
TEST_CASE("D-007 D7: full raster reflection -- the bundle carries the vertex INPUT layout", "[gpu-context][vulkan][gpu][raster][d7]")
{
    namespace sc = crd::shadercook;
    crd::memory::TlsfAllocator alloc(48U << 20U);
    crd::kir::KGraph           g(&alloc);
    crd::kir::KEntry           vs;
    crd::kir::KEntry           fs;
    build_material_program(g, 1U, vs, fs);
    sc::CookOptions opts;
    opts.backends     = static_cast<crd::u32>(sc::CookBackend::SpirV);
    sc::CookResult ck = sc::cook_raster_shader(g, vs, fs, crd::containers::StringView("mat"), opts, &alloc);
    REQUIRE(ck.ok);

    sc::ShaderBundle b(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(ck.crdr), b));

    // vertex input layout (REFV)
    const auto vspan = b.vertex_reflection();
    REQUIRE(vspan.size() == sizeof(crd::kir::ShaderReflection));
    crd::kir::ShaderReflection vrefl{};
    std::memcpy(&vrefl, vspan.data(), sizeof(vrefl));
    CHECK(vrefl.stage == crd::kir::KStage::Vertex);
    REQUIRE(vrefl.n_vattrs == 3); // position, uv, normal
    // the attributes carry location + component count, in order
    CHECK(vrefl.vattrs[0].comps == 3); // position vec3
    CHECK(vrefl.vattrs[1].comps == 2); // uv vec2
    CHECK(vrefl.vattrs[2].comps == 3); // normal vec3

    // fragment descriptor bindings (REFL)
    const auto fspan = b.reflection();
    REQUIRE(fspan.size() == sizeof(crd::kir::ShaderReflection));
    crd::kir::ShaderReflection frefl{};
    std::memcpy(&frefl, fspan.data(), sizeof(frefl));
    CHECK(frefl.stage == crd::kir::KStage::Fragment);
    std::printf("[d7] raster reflection from the IR: %d vertex attributes + %d FS bindings — a complete graphics interface\n",
                vrefl.n_vattrs, frefl.n_bindings);
}

// D-007 D8: the MULTI-VARIANT CONTAINER — cook a whole permutation set into ONE .crdr, key-addressed + content-hash deduped,
// and load each key on-demand. The shipping form of variants: one file, load_variant(key) returns the (shared) bytecode. Uses
// the 4-key/2-unique scale kernel; keys 0/2 share a bundle, 1/3 share a bundle.
TEST_CASE("D-007 D8: multi-variant container -- one .crdr, key-addressed, deduped, on-demand load",
          "[gpu-context][vulkan][gpu][variant][d8]")
{
    namespace sc = crd::shadercook;
    namespace cg = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    const crd::u32  keys[4] = {0U, 1U, 2U, 3U};
    sc::CookOptions opts;
    opts.backends = static_cast<crd::u32>(sc::CookBackend::SpirV);
    sc::CookResult cc = sc::cook_variant_container(&build_scale_variant, nullptr, keys, 4, opts, &alloc);
    REQUIRE(cc.ok);

    sc::VariantContainer vc(&alloc);
    REQUIRE(sc::read_variant_container(crd::containers::as_const_span(cc.crdr), vc));
    CHECK(vc.requested_count() == 4U);
    CHECK(vc.unique_count() == 2U); // 4 keys → 2 unique bundles, in ONE file
    std::printf("[d8] one .crdr container: %u keys → %u unique bundles (%zu B total bytecode)\n", vc.requested_count(),
                vc.unique_count(), static_cast<size_t>(cc.spirv_bytes));

    // Load-by-key: keys 0 and 2 (dead bit1) resolve to the SAME (shared) bytecode; 1 and 3 to another.
    const auto b0 = vc.bytecode(0U);
    const auto b1 = vc.bytecode(1U);
    const auto b2 = vc.bytecode(2U);
    REQUIRE(!b0.empty());
    REQUIRE(!b1.empty());
    REQUIRE(!b2.empty());
    REQUIRE(b0.size() == b2.size());
    CHECK(std::memcmp(b0.data(), b2.data(), b0.size()) == 0); // key0 and key2 SHARE one bundle
    const bool same01 = (b0.size() == b1.size()) && std::memcmp(b0.data(), b1.data(), b0.size()) == 0;
    CHECK(!same01); // key0 (×1) ≠ key1 (×2)

    // The loaded bytecode is REAL, runnable SPIR-V — dispatch key1's variant (×2) straight from the container.
    auto pipe = compute.create_pipeline_from_spirv(b1, 2, 0U);
    REQUIRE(pipe != nullptr);
    constexpr int ls = 32;
    float         in_h[ls];
    float         out_h[ls];
    for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 1.0F; out_h[i] = 0.0F; }
    float*    host[2] = {in_h, out_h};
    const int lens[2] = {ls, ls};
    crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
    int bad = 0;
    for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[i] * 2.0F) { ++bad; } }
    CHECK(bad == 0);
    std::printf("[d8] loaded key1's variant from the container and ran it on GPU: %d/%d ×2 correct\n", ls - bad, ls);
}

// D-007 D9: NEURAL MATERIAL COMPLETENESS. (1) A full-surface neural material — out_dim=8 so the MLP produces base+metallic+
// roughness+LEARNED NORMAL, all written to the deferred G-buffer. (2) End-to-end training: a small MLP is trained (CPU, SGD)
// to fit a reference surface function; the reconstruction PSNR must converge to a target — proving a neural material learns.
TEST_CASE("D-007 D9: neural material completeness -- learned normal + end-to-end training converges", "[gpu-context][vulkan][gpu][neural][d9]")
{
    namespace nn = crd::kir::neural;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    crd::memory::TlsfAllocator alloc(16U << 20U);

    // (1) A full-surface neural material: out_dim=8 → base(0..2), metallic(3), roughness(4), learned normal(5..7).
    if (vk->cooperative_vector())
    {
        nn::CoopVecMlpConfig fcfg;
        fcfg.out_dim = 8;
        crd::kir::GlslKernel kern(&alloc);
        REQUIRE(nn::emit_neural_surface_fs_glsl(fcfg, kern));
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Fragment, crd::containers::to_view(kern.source), "neural_surf8", &alloc);
        REQUIRE(spv.ok); // a full-surface (incl. learned normal) neural material compiles to Fragment SPIR-V
        std::printf("[d9] full-surface neural material (out_dim=8, learned normal): %zu B Fragment SPIR-V\n", static_cast<size_t>(spv.spirv.size()));
    }

    // (2) End-to-end training: fit a 1-hidden-layer MLP (freq-encoded uv → 3 outputs) to a reference surface function.
    constexpr int in_dim = 8; // 2 frequency bands × 4
    constexpr int hid    = 16;
    constexpr int out    = 3;
    constexpr int grid   = 8; // 8×8 uv samples
    float         w1[hid * in_dim];
    float         b1[hid];
    float         w2[out * hid];
    float         b2[out];
    const auto    rnd = [](int i) { const crd::u32 h = (static_cast<crd::u32>(i) * 2654435761U) ^ 0x9E3779B9U; return (static_cast<float>(h & 0xFFFFU) / 32768.0F - 1.0F) * 0.3F; };
    for (int i = 0; i < hid * in_dim; ++i) { w1[i] = rnd(i); }
    for (int i = 0; i < hid; ++i) { b1[i] = 0.0F; }
    for (int i = 0; i < out * hid; ++i) { w2[i] = rnd(i + 971); }
    for (int i = 0; i < out; ++i) { b2[i] = 0.0F; }
    // reference surface: a smooth RGB pattern of (u,v)
    const auto target = [](float u, float v, int ch) {
        if (ch == 0) { return 0.5F + 0.5F * static_cast<float>(crd::math::sin(6.2831853 * static_cast<double>(u))); }
        if (ch == 1) { return 0.5F + 0.5F * static_cast<float>(crd::math::cos(6.2831853 * static_cast<double>(v))); }
        return u * v;
    };
    const auto encode = [](float u, float v, float* x) {
        for (int k = 0; k < in_dim / 4; ++k) {
            const float f = static_cast<float>(1 << k) * 3.14159265F;
            x[4 * k + 0] = static_cast<float>(crd::math::sin(f * u));
            x[4 * k + 1] = static_cast<float>(crd::math::cos(f * u));
            x[4 * k + 2] = static_cast<float>(crd::math::sin(f * v));
            x[4 * k + 3] = static_cast<float>(crd::math::cos(f * v));
        }
    };
    const auto mse = [&]() {
        float e = 0.0F;
        for (int gy = 0; gy < grid; ++gy) {
            for (int gx = 0; gx < grid; ++gx) {
                const float u = (static_cast<float>(gx) + 0.5F) / grid;
                const float v = (static_cast<float>(gy) + 0.5F) / grid;
                float       x[in_dim];
                encode(u, v, x);
                float h[hid];
                for (int j = 0; j < hid; ++j) { float s = b1[j]; for (int i = 0; i < in_dim; ++i) { s += w1[j * in_dim + i] * x[i]; } h[j] = s > 0.0F ? s : 0.0F; }
                for (int o = 0; o < out; ++o) { float s = b2[o]; for (int j = 0; j < hid; ++j) { s += w2[o * hid + j] * h[j]; } const float d = s - target(u, v, o); e += d * d; }
            }
        }
        return e / static_cast<float>(grid * grid * out);
    };
    const float mse0 = mse();
    const float lr   = 0.15F;
    for (int it = 0; it < 4000; ++it) {
        for (int gy = 0; gy < grid; ++gy) {
            for (int gx = 0; gx < grid; ++gx) {
                const float u = (static_cast<float>(gx) + 0.5F) / grid;
                const float v = (static_cast<float>(gy) + 0.5F) / grid;
                float       x[in_dim];
                encode(u, v, x);
                float h[hid];
                float pre[hid];
                for (int j = 0; j < hid; ++j) { float s = b1[j]; for (int i = 0; i < in_dim; ++i) { s += w1[j * in_dim + i] * x[i]; } pre[j] = s; h[j] = s > 0.0F ? s : 0.0F; }
                float dy[out];
                for (int o = 0; o < out; ++o) { float s = b2[o]; for (int j = 0; j < hid; ++j) { s += w2[o * hid + j] * h[j]; } dy[o] = 2.0F * (s - target(u, v, o)) / static_cast<float>(out); }
                float dh[hid];
                for (int j = 0; j < hid; ++j) { float s = 0.0F; for (int o = 0; o < out; ++o) { s += w2[o * hid + j] * dy[o]; } dh[j] = pre[j] > 0.0F ? s : 0.0F; }
                for (int o = 0; o < out; ++o) { for (int j = 0; j < hid; ++j) { w2[o * hid + j] -= lr * dy[o] * h[j]; } b2[o] -= lr * dy[o]; }
                for (int j = 0; j < hid; ++j) { for (int i = 0; i < in_dim; ++i) { w1[j * in_dim + i] -= lr * dh[j] * x[i]; } b1[j] -= lr * dh[j]; }
            }
        }
    }
    const float mse1 = mse();
    const float psnr = static_cast<float>(-10.0 * crd::math::log10(static_cast<double>(mse1 < 1e-12F ? 1e-12F : mse1)));
    std::printf("[d9] neural material training: MSE %.5f -> %.6f, reconstruction PSNR %.1f dB\n", static_cast<double>(mse0), static_cast<double>(mse1), static_cast<double>(psnr));
    CHECK(mse1 < mse0 * 0.2F); // training converged
    CHECK(psnr >= 20.0F);      // the learned surface reproduces the reference
}

// D-007 D10: PARALLEL COOK on the fiber-based crd-jobs scheduler (NOT a bespoke pool). Cook a variant matrix concurrently and
// prove it's byte-identical to the serial cook (content-hash dedup is order-independent) — each job on its own allocator,
// writing content-addressed to the cache. More variants → more parallelism, deterministic result.
TEST_CASE("D-007 D10: parallel cook on crd-jobs is byte-identical to the serial cook", "[gpu-context][gpu][variant][d10]")
{
    namespace sc = crd::shadercook;
    crd::memory::TlsfAllocator alloc(64U << 20U);

    // 8 keys (bit0 = scale) → 2 unique. Cook serially, then in parallel, into two cache dirs; compare the produced files.
    const crd::u32 keys[8] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    const char*    scdir   = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/cook-serial";
    const char*    pcdir   = "C:/Users/abici/AppData/Local/Temp/claude/D--Dev-cerid/b0138d6a-548b-428b-87b2-fe30c9f36f7c/scratchpad/cook-parallel";
    (void)crd::platform::fs::create_directories(crd::platform::fs::Path(scdir));
    (void)crd::platform::fs::create_directories(crd::platform::fs::Path(pcdir));

    sc::CookOptions so;
    so.backends  = static_cast<crd::u32>(sc::CookBackend::SpirV);
    so.cache_dir = scdir;
    sc::VariantMatrixResult serial = sc::cook_variant_matrix(&build_scale_variant, nullptr, keys, 8, so, &alloc);
    REQUIRE(serial.ok);

    crd::jobs::init(crd::jobs::Config{.num_threads = 4});
    sc::CookOptions po;
    po.backends  = static_cast<crd::u32>(sc::CookBackend::SpirV);
    po.cache_dir = pcdir;
    sc::VariantMatrixResult par = sc::cook_variant_matrix_parallel(&build_scale_variant, nullptr, keys, 8, po, &alloc);
    crd::jobs::shutdown();
    REQUIRE(par.ok);

    // Same telemetry, same manifest (key → hash).
    CHECK(serial.requested == par.requested);
    CHECK(serial.unique == par.unique);
    REQUIRE(serial.entries.size() == par.entries.size());
    bool manifest_match = true;
    for (crd::usize i = 0; i < serial.entries.size(); ++i)
    {
        if (!(serial.entries[i].hash == par.entries[i].hash) || serial.entries[i].key != par.entries[i].key) { manifest_match = false; }
    }
    CHECK(manifest_match);

    // Each unique cache file the parallel cook wrote is BYTE-IDENTICAL to the serial one.
    int checked = 0;
    for (crd::usize i = 0; i < serial.entries.size(); ++i)
    {
        crd::containers::String idstr = serial.entries[i].hash.to_string(&alloc);
        char                    suffix[24];
        std::snprintf(suffix, sizeof(suffix), "_%08x.crdr", static_cast<crd::u32>(sc::CookBackend::SpirV));
        crd::containers::String sp(&alloc);
        sp.append(scdir); sp.append("/"); sp.append(idstr.c_str()); sp.append(suffix);
        crd::containers::String pp(&alloc);
        pp.append(pcdir); pp.append("/"); pp.append(idstr.c_str()); pp.append(suffix);
        crd::containers::Array<crd::u8> sb(&alloc);
        crd::containers::Array<crd::u8> pb(&alloc);
        if (crd::platform::fs::read_file_binary(crd::platform::fs::Path(sp.c_str()), sb)
            && crd::platform::fs::read_file_binary(crd::platform::fs::Path(pp.c_str()), pb))
        {
            REQUIRE(sb.size() == pb.size());
            CHECK(std::memcmp(sb.data(), pb.data(), sb.size()) == 0);
            ++checked;
        }
    }
    CHECK(checked >= 1);
    std::printf("[d10] parallel cook (crd-jobs, 4 workers): 8 keys -> %u unique, manifest matches serial, %d cache files byte-identical\n",
                par.unique, checked);
}

// D-007 D11 (ADR-0104): ASYNC PIPELINE WARMUP. The render-thread payoff of the deploy chain — build a batch of live pipelines
// OFF the render thread on the fiber scheduler (crd-jobs, NOT a bespoke pool). submit() is non-blocking; the main thread does
// other work while a background worker compiles SPIR-V→ISA; wait() joins and the pipelines are hot. Proven by: submit() returns
// immediately (main-thread work runs before the warm finishes), all requested pipelines build, each is dispatch-correct on the
// render thread (a VkPipeline built on a worker binds anywhere), and key lookup maps a variant key → its ready pipeline.
TEST_CASE("D-007 D11: async pipeline warmup on crd-jobs -- pipelines built off the render thread, dispatch-correct",
          "[gpu-context][vulkan][gpu][d11]")
{
    namespace sc = crd::shadercook;
    namespace cg = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    // Cook two variants (scale x1 @ key0, x2 @ key1); hold the CookResults + bundles so the SPIR-V spans stay valid through wait().
    sc::CookOptions opts;
    opts.backends = static_cast<crd::u32>(sc::CookBackend::SpirV);
    sc::CookResult r0 = sc::cook_one_variant(&build_scale_variant, nullptr, 0U, crd::containers::StringView("v0"), opts, &alloc);
    sc::CookResult r1 = sc::cook_one_variant(&build_scale_variant, nullptr, 1U, crd::containers::StringView("v1"), opts, &alloc);
    REQUIRE(r0.ok);
    REQUIRE(r1.ok);
    sc::ShaderBundle b0(&alloc);
    sc::ShaderBundle b1(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(r0.crdr), b0));
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(r1.crdr), b1));

    crd::jobs::init(crd::jobs::Config{.num_threads = 4});

    // Queue the warm, kick it (non-blocking), then do UNRELATED main-thread work while a worker compiles the pipelines.
    sc::AsyncPipelineWarmer warmer(&alloc);
    warmer.add(b0.bytecode(sc::CookBackend::SpirV), 2, 0U);
    warmer.add(b1.bytecode(sc::CookBackend::SpirV), 2, 1U);
    warmer.submit(&vk_create_from_spirv, &compute); // reuses the D5 create-callback (context = user)
    REQUIRE(warmer.in_flight());                    // submit() did NOT block — the warm is running on a worker
    crd::u64 spin = 0U;                             // main-thread work overlapping the background compile (must not touch `compute`)
    for (crd::u32 i = 0U; i < 200000U; ++i) { spin += (i * 2654435761U) ^ (spin >> 3U); }
    warmer.wait();                                  // join — the batch is hot
    CHECK(spin != 0U);                              // (defeat dead-code elimination of the overlap work)

    REQUIRE(warmer.count() == 2U);
    CHECK(warmer.warmed() == 2U);
    REQUIRE(warmer.pipeline(0U) != nullptr);
    REQUIRE(warmer.pipeline(1U) != nullptr);
    CHECK(warmer.pipeline_for_key(0U) == warmer.pipeline(0U));
    CHECK(warmer.pipeline_for_key(1U) == warmer.pipeline(1U));

    // Each worker-built pipeline dispatches correctly ON THE RENDER (main) thread.
    const auto run = [&](cg::ComputePipeline* pipe, float expect_scale) {
        constexpr int ls = 32;
        float         in_h[ls];
        float         out_h[ls];
        for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 1.0F; out_h[i] = 0.0F; }
        float*    host[2] = {in_h, out_h};
        const int lens[2] = {ls, ls};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
        int bad = 0;
        for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[i] * expect_scale) { ++bad; } }
        return bad;
    };
    CHECK(run(warmer.pipeline_for_key(0U), 1.0F) == 0); // x1
    CHECK(run(warmer.pipeline_for_key(1U), 2.0F) == 0); // x2

    crd::jobs::shutdown();
    std::printf("[d11] async warmup: 2 pipelines built on a crd-jobs worker off the render thread (submit non-blocking), "
                "both dispatch-correct on the main thread\n");
}

// D-007 D12: a kernel whose scale is a SPECIALIZATION CONSTANT (id 0, default 1.0) — outb[lid] = inb[lid] * spec0. The spec
// constant is NOT baked at cook; it stays in the SPIR-V as an OpSpecConstant the pipeline sets at build time.
namespace
{
crd::kir::KEntry build_spec_scale_kernel(crd::kir::KGraph& g)
{
    namespace k       = crd::kir;
    const int  inbuf  = g.buffer_decl(k::DType::F32, 0, 0, false);
    const int  outbuf = g.buffer_decl(k::DType::F32, 0, 1, true);
    const int  lid    = g.builtin(k::KBuiltin::LocalInvocationIndex);
    const int  scale  = g.spec_constant(0U, 1.0, k::DType::F32); // pipeline-time scale, default 1.0 (constant_id = 0)
    const int  mark   = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid, g.binary(k::KOp::Mul, g.buffer_load(inbuf, lid), scale));
    k::KEntry e;
    e.stage             = k::KStage::Compute;
    e.local_size[0]     = 32;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

// D-007 D12 (ADR-0104): SPEC-CONSTANT BINDING AT LOAD — ONE cooked bundle serves MANY runtime-cheap variants. Cook a kernel with
// a specialization-constant scale ONCE, then build SEVERAL pipelines from the SAME cooked SPIR-V, each binding a different value
// via VkSpecializationInfo (the driver folds it into the ISA at pipeline-creation) — no recook, no separate bundle. Contrast the
// D3 variant matrix, which cooks a distinct bundle per value; spec constants are the cheaper axis for values that don't change
// the control flow. Proven: 4 different scales (incl. the unbound DEFAULT) all run GPU-correct from one bundle.
TEST_CASE("D-007 D12: spec constants -- one cooked bundle, many pipelines via VkSpecializationInfo", "[gpu-context][vulkan][gpu][d12]")
{
    namespace sc  = crd::shadercook;
    namespace kir = crd::kir;
    namespace cg  = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(32U << 20U);

    // COOK ONCE. The spec constant survives cooking (it is not const-folded), so the SPIR-V carries an OpSpecConstant.
    kir::KGraph       g(&alloc);
    const kir::KEntry e = build_spec_scale_kernel(g);
    sc::CookOptions   opts;
    opts.backends     = static_cast<crd::u32>(sc::CookBackend::SpirV);
    sc::CookResult ck = sc::cook_compute_shader(g, e, crd::containers::StringView("spec_scale"), opts, &alloc);
    REQUIRE(ck.ok);
    CHECK(ck.spirv_bytes > 0U);
    sc::ShaderBundle b(&alloc);
    REQUIRE(sc::read_shader_bundle(crd::containers::as_const_span(ck.crdr), b));
    const auto spv = b.bytecode(sc::CookBackend::SpirV);
    REQUIRE(!spv.empty());

    using SC = cg::VulkanComputeContext::SpecConstantBinding;
    const auto run_with_scale = [&](cg::ComputePipeline* pipe, float expect_scale) {
        REQUIRE(pipe != nullptr);
        constexpr int ls = 32;
        float         in_h[ls];
        float         out_h[ls];
        for (int i = 0; i < ls; ++i) { in_h[i] = static_cast<float>(i) + 1.0F; out_h[i] = 0.0F; }
        float*    host[2] = {in_h, out_h};
        const int lens[2] = {ls, ls};
        crd::kir_test::dispatch_kernel_1wg(compute, *pipe, host, lens, 2, 1U);
        int bad = 0;
        for (int i = 0; i < ls; ++i) { if (out_h[i] != in_h[i] * expect_scale) { ++bad; } }
        return bad;
    };
    // Four pipelines from the SAME cooked bytecode — three explicit spec values + the unbound default — all correct, no recompile.
    const float scales[3] = {2.0F, 3.5F, 0.25F};
    for (float sv : scales)
    {
        crd::u32 bits = 0U;
        std::memcpy(&bits, &sv, 4);
        SC       specs[1] = {{0U, bits}};
        auto     pipe     = compute.create_pipeline_from_spirv(spv, 2, 0U, crd::containers::ConstSpan<SC>(specs, 1));
        CHECK(run_with_scale(pipe.get(), sv) == 0);
    }
    auto pdef = compute.create_pipeline_from_spirv(spv, 2, 0U); // NO spec binding → the constant's DEFAULT (1.0)
    CHECK(run_with_scale(pdef.get(), 1.0F) == 0);

    // ONE bundle: re-cooking is content-hash identical regardless of any runtime spec value (the value is not in the IR).
    sc::CookResult ck2 = sc::cook_compute_shader(g, e, crd::containers::StringView("spec_scale"), opts, &alloc);
    REQUIRE(ck2.ok);
    REQUIRE(ck.crdr.size() == ck2.crdr.size());
    CHECK(std::memcmp(ck.crdr.data(), ck2.crdr.data(), ck.crdr.size()) == 0);
    std::printf("[d12] one cooked bundle (%u B SPIR-V) served scale 2.0 / 3.5 / 0.25 + default(1.0) via VkSpecializationInfo "
                "(zero recook)\n", ck.spirv_bytes);
}

// D-007 AS-6b (ADR-0098 §4): AUTOTUNE THE VULKAN/SPIR-V GEMM. `emit_contract_tiled_glsl_sched` is the parameterized GLSL GEMM
// (block BT×BT, K-depth BK, TM×TM register microtile), so the autotuner drives Vulkan exactly like CUDA: enumerate (BT,BK,TM),
// compile GLSL→SPIR-V, dispatch + `last_gpu_ms`-time, oracle-validate each, keep the fastest CORRECT. Proves the auto-scheduler
// is a CROSS-BACKEND compiler property (the Vulkan device gets its own tuned schedule; AS-6a device-keys the DB).
namespace
{
float as6_av(int i, int k) { return static_cast<float>((i * 7 + k) % 13) * 0.01F - 0.06F; }
float as6_bv(int k, int j) { return static_cast<float>((k * 5 + j) % 11) * 0.008F - 0.04F; }
bool  as6_correct(const float* cbuf, int mm, int nn, int kk)
{
    float maxrel = 0.0F;
    for (int s = 0; s < 512; ++s)
    {
        const int i   = (s * 977) % mm;
        const int j   = (s * 1471) % nn;
        double    acc = 0.0;
        for (int k = 0; k < kk; ++k) { acc += static_cast<double>(as6_av(i, k)) * static_cast<double>(as6_bv(k, j)); }
        const float ref = static_cast<float>(acc);
        const float got = cbuf[static_cast<crd::usize>(i) * nn + j];
        const float rel = (got - ref) / (1.0F + (ref < 0.0F ? -ref : ref));
        const float ar  = rel < 0.0F ? -rel : rel;
        if (ar > maxrel) { maxrel = ar; }
    }
    return maxrel < 3e-3F;
}
} // namespace

TEST_CASE("D-007 AS-6b: autotune the Vulkan/SPIR-V GEMM -- parameterized GLSL schedule + last_gpu_ms search, oracle-gated",
          "[gpu-context][vulkan][gpu][autotune]")
{
    namespace cg = crd::gpu;
    gpu::GpuContextConfig cfg;
    cfg.backend  = gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    cg::VulkanComputeContext compute(*vk, crd::memory::default_allocator());
    REQUIRE(compute.valid());
    crd::memory::TlsfAllocator alloc(128U << 20U);
    using cg::compute_usage::storage;
    using cg::compute_usage::transfer_dst;
    using cg::compute_usage::transfer_src;

    constexpr int   mm = 512;
    constexpr int   nn = 512;
    constexpr int   kk = 512;
    crd::kir::KGraph g(&alloc);
    const int        a = g.input(crd::kir::make_shape({mm, kk}), crd::kir::DType::F32);
    const int        b = g.input(crd::kir::make_shape({kk, nn}), crd::kir::DType::F32);
    const int        c = g.contract(a, b);

    crd::containers::Array<float> h_a(&alloc);
    crd::containers::Array<float> h_b(&alloc);
    crd::containers::Array<float> h_c(&alloc);
    h_a.resize(static_cast<crd::usize>(mm) * kk);
    h_b.resize(static_cast<crd::usize>(kk) * nn);
    h_c.resize(static_cast<crd::usize>(mm) * nn);
    for (int i = 0; i < mm; ++i) { for (int k = 0; k < kk; ++k) { h_a[static_cast<crd::usize>(i) * kk + k] = as6_av(i, k); } }
    for (int k = 0; k < kk; ++k) { for (int j = 0; j < nn; ++j) { h_b[static_cast<crd::usize>(k) * nn + j] = as6_bv(k, j); } }

    auto d_a = compute.create_buffer(static_cast<crd::u64>(mm) * kk * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_b = compute.create_buffer(static_cast<crd::u64>(kk) * nn * 4U, storage | transfer_dst, cg::ComputeMemory::GpuOnly);
    auto d_c = compute.create_buffer(static_cast<crd::u64>(mm) * nn * 4U, storage | transfer_src, cg::ComputeMemory::GpuOnly);
    const auto upload = [&](cg::ComputeBuffer& dst, const void* src, crd::u64 nb) {
        auto        stg = compute.create_buffer(nb, transfer_src, cg::ComputeMemory::CpuToGpu);
        auto*       p   = static_cast<crd::u8*>(stg->map());
        const auto* ss  = static_cast<const crd::u8*>(src);
        for (crd::u64 i = 0; i < nb; ++i) { p[i] = ss[i]; }
        stg->unmap();
        auto& rc = compute.begin();
        rc.copy(*stg, dst, 0U, 0U, nb);
        compute.submit_and_wait();
    };
    upload(*d_a, h_a.data(), static_cast<crd::u64>(mm) * kk * 4U);
    upload(*d_b, h_b.data(), static_cast<crd::u64>(kk) * nn * 4U);

    struct Cand { int bt; int bk; int tm; };
    const Cand     cands[] = {{64, 8, 4}, {64, 16, 4}, {128, 8, 8}, {128, 16, 8}, {64, 8, 8}, {32, 8, 4}, {128, 8, 4}};
    const crd::u32 pc[4]   = {static_cast<crd::u32>(mm), static_cast<crd::u32>(kk), static_cast<crd::u32>(nn), 1U};
    double         best_ms = 1.0e30;
    Cand           best{0, 0, 0};
    int            measured = 0;
    int            correct  = 0;
    auto           rb       = compute.create_buffer(static_cast<crd::u64>(mm) * nn * 4U, transfer_dst, cg::ComputeMemory::GpuToCpu);
    for (const Cand& cd : cands)
    {
        if ((mm % cd.bt) != 0 || (nn % cd.bt) != 0 || (kk % cd.bk) != 0) { continue; }
        crd::kir::GlslKernel kern(&alloc);
        if (!crd::kir::emit_contract_tiled_glsl_sched(g, c, cd.bt, cd.bk, cd.tm, kern)) { continue; }
        const auto spv = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), "gemm", &alloc, true);
        if (!spv.ok) { continue; }
        auto pipe = compute.create_pipeline_from_spirv(crd::containers::ConstSpan<crd::u8>(spv.spirv.data(), spv.spirv.size()), 3, 16U);
        if (pipe == nullptr) { continue; }
        const crd::u32     gx      = static_cast<crd::u32>((mm / cd.bt) * (nn / cd.bt));
        cg::ComputeBuffer* binds[3] = {d_a.get(), d_b.get(), d_c.get()};
        for (int w = 0; w < 2; ++w)
        {
            auto& r = compute.begin();
            r.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 3), pc, 16U, gx, 1U, 1U);
            compute.submit_and_wait();
        }
        double mn = 1.0e30;
        for (int it = 0; it < 8; ++it)
        {
            auto& r = compute.begin();
            r.dispatch(*pipe, crd::containers::ConstSpan<cg::ComputeBuffer*>(binds, 3), pc, 16U, gx, 1U, 1U);
            r.barrier(*d_c, cg::ComputeAccess::ShaderWrite, cg::ComputeAccess::TransferSrc);
            compute.submit_and_wait();
            const double ms = compute.last_gpu_ms();
            if (ms > 0.0 && ms < mn) { mn = ms; }
        }
        {
            auto& r = compute.begin();
            r.copy(*d_c, *rb, 0U, 0U, static_cast<crd::u64>(mm) * nn * 4U);
            compute.submit_and_wait();
        }
        const auto* cptr = static_cast<const float*>(rb->map());
        for (crd::usize i = 0; i < h_c.size(); ++i) { h_c[i] = cptr[i]; }
        rb->unmap();
        ++measured;
        if (!as6_correct(h_c.data(), mm, nn, kk)) { continue; }
        ++correct;
        if (mn < best_ms) { best_ms = mn; best = cd; }
    }
    REQUIRE(best.bt > 0);
    CHECK(correct == measured); // every emittable GLSL schedule computes correctly
    const double gflops = 2.0 * static_cast<double>(mm) * nn * kk / (best_ms * 1.0e6);
    std::printf("[AS-6b] Vulkan GEMM %dx%dx%d: autotuned BT%d BK%d TM%d -> %.3f ms (%.0f GFLOP/s) from %d/%d correct GLSL schedules\n",
                mm, nn, kk, best.bt, best.bk, best.tm, best_ms, gflops, correct, measured);
    // the throughput FLOOR is a GPU-class claim: on a CPU implementation (llvmpipe — deviceType CPU) the
    // autotuner's own guarantees (enumerate, oracle-validate, pick fastest) are asserted above; the floor
    // drops to "it really computed" because no software rasterizer owes GPU throughput.
    VkPhysicalDeviceProperties as6_props{};
    vkGetPhysicalDeviceProperties(vk->vk_physical_device(), &as6_props);
    const bool as6_cpu_device = as6_props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    CHECK(gflops > (as6_cpu_device ? 1.0 : 200.0)); // GPU: the tiled+autotuned GEMM is real (naive is ~tens of GFLOP/s)
}

// ═══ GEO-1 (D-007 row 66): THE DRAW GATE — an IMPORTED file renders through the engine ═════════════════════════════════
// The full chain, end to end: millimetre-authored STL bytes → the wave1 cook handler (crd-asset-io parse → the
// crd-geometry validate hook → the 48-byte interleave with `.meta position_scale` mm→m) → `crdr_read` the cooked MESH
// artifact → `upload_storage` (the NEW vertex-feeding path) → a VERTEX-PULLING VS (storage_load by VertexIndex against
// the cooked stride) → flat-red FS → readback. The cooked quad covers the LEFT half of clip space ⇒ left = red, right =
// the blue clear — and the geometry is only correct if the mm→m cook scale actually landed (unscaled positions would be
// ±1000 clip units: the quad would cover EVERYTHING). The first time a real imported file draws through Cerid.

namespace crd::cooker
{
void register_wave1_mesh_handler(); // mesh_wave1.cpp (crd-cooker)
} // namespace crd::cooker

TEST_CASE("GEO-1: an IMPORTED STL cooks, uploads, and DRAWS via vertex pulling (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][geo]")
{
    namespace kir = crd::kir;
    namespace pfs = crd::platform::fs;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    // 1. the source: a left-half-of-clip-space quad, authored in MILLIMETRES (2 CCW triangles, +Z normals)
    crd::containers::Array<crd::u8> stl(&alloc);
    {
        const auto pushf = [&](crd::f32 v) {
            crd::u8 raw[4];
            std::memcpy(raw, &v, 4);
            for (crd::u8 x : raw) { stl.push_back(x); }
        };
        const auto pushtri = [&](const crd::f32* a, const crd::f32* b, const crd::f32* c) {
            const crd::f32 nz[3] = {0.0F, 0.0F, 1.0F};
            for (int i = 0; i < 3; ++i) { pushf(nz[i]); }
            for (int i = 0; i < 3; ++i) { pushf(a[i]); }
            for (int i = 0; i < 3; ++i) { pushf(b[i]); }
            for (int i = 0; i < 3; ++i) { pushf(c[i]); }
            stl.push_back(0);
            stl.push_back(0);
        };
        for (int i = 0; i < 80; ++i) { stl.push_back(0); } // header
        const crd::u32 count = 2;
        crd::u8        raw[4];
        std::memcpy(raw, &count, 4);
        for (crd::u8 x : raw) { stl.push_back(x); }
        const crd::f32 p00[3] = {-1000.0F, -1000.0F, 0.0F};
        const crd::f32 p10[3] = {0.0F, -1000.0F, 0.0F};
        const crd::f32 p11[3] = {0.0F, 1000.0F, 0.0F};
        const crd::f32 p01[3] = {-1000.0F, 1000.0F, 0.0F};
        pushtri(p00, p10, p11);
        pushtri(p00, p11, p01);
    }
    const char* src_path  = "cerid_geo1_draw.stl";
    const char* meta_path = "cerid_geo1_draw.stl.meta";
    REQUIRE(pfs::write_file_binary(pfs::Path(crd::containers::StringView(src_path)), crd::containers::as_const_span(stl)));
    REQUIRE(pfs::write_file_text(pfs::Path(crd::containers::StringView(meta_path)),
                                 crd::containers::StringView("[cook]\nposition_scale = 0.001\n")));

    // 2. COOK through the real wave1 handler (register once for this binary)
    static bool s_registered = false;
    if (!s_registered)
    {
        crd::cooker::register_wave1_mesh_handler();
        s_registered = true;
    }
    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".stl"));
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext cctx;
    cctx.source_path = crd::containers::StringView(src_path);
    cctx.meta_path   = crd::containers::StringView(meta_path);
    cctx.id          = crd::resources::ResourceId::mint_random();
    cctx.allocator   = &alloc;
    crd::cooker::CookIO cctx_io(cctx.source_path, cctx.meta_path, &alloc); // GEO-6: the only road to bytes
    cctx.io          = &cctx_io;
    const crd::cooker::CookResult cooked = handler(cctx);
    REQUIRE(cooked.ok);

    // 3. the cooked MESH artifact → the vertex stream
    crd::resources::CrdrFile file(&alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(cooked.cooked_bytes), file, &alloc)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* vert = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_VERT);
    const crd::resources::CrdrChunk* indx = crd::resources::crdr_find_chunk(file, crd::resources::kFourCC_INDX);
    REQUIRE(vert != nullptr);
    REQUIRE(indx != nullptr);
    REQUIRE(vert->payload.size() == 4U * 48U); // GEO-2: the soup quad WELDS to 4 indexed vertices at the cooked stride
    REQUIRE(indx->payload.size() == 6U * 4U);  // ... drawn through 6 indices
    // CPU-expand the index buffer into the pulled stream (a non-indexed vertex-pull draw; GPU-side indexed pulling is the
    // GEO-7 render-path item — a second storage binding for the index buffer)
    crd::containers::Array<crd::u8> stream(&alloc);
    stream.reserve(6U * 48U);
    for (crd::u32 ii = 0; ii < 6U; ++ii)
    {
        crd::u32 vi = 0;
        std::memcpy(&vi, indx->payload.data() + ii * 4U, 4U);
        REQUIRE(vi < 4U);
        for (crd::u32 b = 0; b < 48U; ++b) { stream.push_back(vert->payload[vi * 48U + b]); }
    }

    // 4. the DRAW: vertex-pulling VS + flat-red FS; the cooked stream uploaded into the storage buffer
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_vertex_pull_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe); // constant red

    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr); // the VERTEX stage emits the readonly SSBO decl + compiles (the GEO-1 emitter extension)
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 64U;
    auto               target = r.raster->create_color_target(dim, dim);
    auto storage = r.raster->create_storage_buffer(static_cast<crd::u32>(stream.size()));
    REQUIRE(target != nullptr);
    REQUIRE(storage != nullptr);
    REQUIRE(r.raster->upload_storage(*storage, 0U, stream.data(),
                                     static_cast<crd::u32>(stream.size()))); // the NEW vertex-feeding upload
    r.raster->draw_storage(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *storage, 6U);

    // 5. the gate: the left half (the imported quad, mm→m scaled to x ∈ [-1,0]) is RED; the right half stays BLUE
    const crd::u32 left  = target->read_pixel(dim / 4U, dim / 2U);
    const crd::u32 right = target->read_pixel((3U * dim) / 4U, dim / 2U);
    const crd::u32 lr    = left & 0xFFU;
    const crd::u32 lb    = (left >> 16U) & 0xFFU;
    const crd::u32 rr    = right & 0xFFU;
    const crd::u32 rb    = (right >> 16U) & 0xFFU;
    WARN("[geo1-draw vulkan] left=(" << lr << ",b" << lb << ") right=(" << rr << ",b" << rb << ")");
    CHECK(lr > 200U); // imported geometry covers the left half — RED
    CHECK(lb < 50U);
    CHECK(rr < 50U); // the right half is untouched clear — BLUE (also proves the mm→m scale: unscaled would cover ALL)
    CHECK(rb > 200U);

    (void)pfs::remove_file(pfs::Path(crd::containers::StringView(src_path)));
    (void)pfs::remove_file(pfs::Path(crd::containers::StringView(meta_path)));
}

// ═══ GEO-3 CLOSE (D-007 row 68): a TEXTURED glTF decomposes into 4 native artifact types and RENDERS through the ═══════
// gpu-context stack (ADR-0105's one graphics layer). The chain: a GLB (embedded 2×2 black/white-checker PNG as
// baseColorTexture + an authored baseColorFactor) → the wave1 cook (MESH + TXTR + PBRM + SCEN artifacts) → the cooked
// TXTR's mip chain uploads VERBATIM via `create_texture_from_mips` (sRGB) → a CKIR FS texelFetches MIP 1 and modulates
// by the LOADED material's base_color → readback. The observable is razor-sharp: the cooked linear-space mip-1 byte is
// 188 → hardware sRGB decode ≈ 0.503 → × (0.5, 1.0, 0.25) → target bytes ≈ (64, 128, 32). A device-side re-derived
// byte-box mip (127) would decode to 0.216 and land at (28, 55, 14) — the two pipelines are unconfusable.

TEST_CASE("GEO-3 CLOSE: a textured glTF decomposes (MESH+TXTR+PBRM+SCEN) and RENDERS through gpu-context (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][geo]")
{
    namespace kir = crd::kir;
    namespace pfs = crd::platform::fs;
    auto        r = vk_raster_or_skip();
    if (r.vk == nullptr) { WARN("no Vulkan device / VK_EXT_shader_object; skipping"); return; }
    REQUIRE(r.raster != nullptr);
    crd::memory::TlsfAllocator alloc(16U << 20U);

    // 1. the source: a GLB — 1 triangle + an EMBEDDED 2×2 checker PNG (white,black / black,white) bound as the
    //    baseColorTexture of a material with baseColorFactor (0.5, 1.0, 0.25); one node referencing the mesh
    crd::containers::Array<crd::u8> png(&alloc);
    {
        const auto push_be = [&](crd::containers::Array<crd::u8>& b, crd::u32 v) {
            b.push_back(static_cast<crd::u8>(v >> 24U));
            b.push_back(static_cast<crd::u8>(v >> 16U));
            b.push_back(static_cast<crd::u8>(v >> 8U));
            b.push_back(static_cast<crd::u8>(v));
        };
        const auto add_chunk = [&](const char* type, const crd::containers::Array<crd::u8>& payload) {
            push_be(png, static_cast<crd::u32>(payload.size()));
            crd::containers::Array<crd::u8> crc_in(&alloc);
            for (int i = 0; i < 4; ++i) { crc_in.push_back(static_cast<crd::u8>(type[i])); }
            for (crd::usize i = 0; i < payload.size(); ++i) { crc_in.push_back(payload[i]); }
            for (int i = 0; i < 4; ++i) { png.push_back(static_cast<crd::u8>(type[i])); }
            for (crd::usize i = 0; i < payload.size(); ++i) { png.push_back(payload[i]); }
            push_be(png, crd::resources::png_crc32(crd::containers::as_const_span(crc_in)));
        };
        const crd::u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        for (crd::u8 s : sig) { png.push_back(s); }
        crd::containers::Array<crd::u8> ihdr(&alloc);
        push_be(ihdr, 2U);
        push_be(ihdr, 2U);
        ihdr.push_back(8);
        ihdr.push_back(6); // RGBA8
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        add_chunk("IHDR", ihdr);
        crd::containers::Array<crd::u8> raw(&alloc); // 2 scanlines, filter 0: (white, black) / (black, white)
        const crd::u8 rows[2][9] = {{0, 255, 255, 255, 255, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 255, 255, 255, 255}};
        for (const auto& row : rows)
        {
            for (crd::u8 b : row) { raw.push_back(b); }
        }
        auto idat = crd::resources::zlib_deflate(crd::containers::as_const_span(raw), &alloc);
        add_chunk("IDAT", idat);
        crd::containers::Array<crd::u8> iend(&alloc);
        add_chunk("IEND", iend);
    }

    crd::containers::Array<crd::u8> bin(&alloc);
    {
        const auto pushf = [&](crd::f32 v) {
            crd::u8 raw4[4];
            std::memcpy(raw4, &v, 4);
            for (crd::u8 x : raw4) { bin.push_back(x); }
        };
        const crd::f32 pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        for (crd::f32 v : pos) { pushf(v); }
        for (crd::usize i = 0; i < png.size(); ++i) { bin.push_back(png[i]); }
    }

    crd::containers::String json(&alloc);
    json.append(R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],)");
    {
        char buf[256];
        (void)std::snprintf(buf, sizeof(buf),
                            R"("buffers": [{"byteLength": %u}],)"
                            R"("bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},)"
                            R"({"buffer": 0, "byteOffset": 36, "byteLength": %u}],)",
                            static_cast<crd::u32>(bin.size()), static_cast<crd::u32>(png.size()));
        json.append(buf);
    }
    json.append(R"("accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"}],)"
                R"("images": [{"name": "checker", "bufferView": 1}],)"
                R"("textures": [{"source": 0}],)"
                R"("materials": [{"name": "tinted", "pbrMetallicRoughness": {)"
                R"("baseColorFactor": [0.5, 1.0, 0.25, 1.0], "baseColorTexture": {"index": 0}}}],)"
                R"("nodes": [{"name": "obj", "mesh": 0}],)"
                R"("meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}]})");

    crd::containers::Array<crd::u8> glb(&alloc);
    {
        const auto pushu = [&](crd::u32 v) {
            crd::u8 raw4[4];
            std::memcpy(raw4, &v, 4);
            for (crd::u8 x : raw4) { glb.push_back(x); }
        };
        const crd::u32 jlen = static_cast<crd::u32>(json.size());
        const crd::u32 jpad = (4U - (jlen % 4U)) % 4U;
        const crd::u32 blen = static_cast<crd::u32>(bin.size());
        const crd::u32 bpad = (4U - (blen % 4U)) % 4U;
        pushu(0x46546C67U);
        pushu(2U);
        pushu(12U + 8U + jlen + jpad + 8U + blen + bpad);
        pushu(jlen + jpad);
        pushu(0x4E4F534AU);
        for (crd::u32 i = 0; i < jlen; ++i) { glb.push_back(static_cast<crd::u8>(json.c_str()[i])); }
        for (crd::u32 i = 0; i < jpad; ++i) { glb.push_back(' '); }
        pushu(blen + bpad);
        pushu(0x004E4942U);
        for (crd::usize i = 0; i < bin.size(); ++i) { glb.push_back(bin[i]); }
        for (crd::u32 i = 0; i < bpad; ++i) { glb.push_back(0); }
    }

    const char* src_path = "cerid_geo3_close.glb";
    REQUIRE(pfs::write_file_binary(pfs::Path(crd::containers::StringView(src_path)), crd::containers::as_const_span(glb)));

    // 2. COOK: the FULL decompose — MESH (main) + TXTR + PBRM + SCEN extras
    static bool s_geo3_registered = false;
    if (!s_geo3_registered)
    {
        crd::cooker::register_wave1_mesh_handler();
        s_geo3_registered = true;
    }
    crd::cooker::CookHandlerFn handler = crd::cooker::find_cook_handler(crd::containers::StringView(".glb"));
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext cctx;
    cctx.source_path = crd::containers::StringView(src_path);
    cctx.id          = crd::resources::ResourceId::mint_random();
    cctx.allocator   = &alloc;
    crd::cooker::CookIO cctx_io(cctx.source_path, cctx.meta_path, &alloc); // GEO-6: the only road to bytes
    cctx.io          = &cctx_io;
    const crd::cooker::CookResult cooked = handler(cctx);
    REQUIRE(cooked.ok);
    REQUIRE(cooked.extra_artifacts.size() == 3U); // TXTR + PBRM + SCEN — every resource type, natively decomposed
    const crd::cooker::ExtraArtifact* txtr = nullptr;
    const crd::cooker::ExtraArtifact* pbrm = nullptr;
    const crd::cooker::ExtraArtifact* scen = nullptr;
    for (crd::usize i = 0; i < cooked.extra_artifacts.size(); ++i)
    {
        const auto& e = cooked.extra_artifacts[i];
        if (e.type_fourcc == crd::resources::kFourCC_TXTR) { txtr = &e; }
        if (e.type_fourcc == crd::resources::kFourCC_PBRM) { pbrm = &e; }
        if (e.type_fourcc == crd::scene::kFourCC_SCEN) { scen = &e; }
    }
    REQUIRE(txtr != nullptr);
    REQUIRE(pbrm != nullptr);
    REQUIRE(scen != nullptr);

    // 3. the AUTHORED material: params verbatim + the base_color slot references the cooked TXTR
    crd::resources::OpenPbrMaterialLoader mloader;
    crd::resources::LoadContext           mctx;
    mctx.id        = pbrm->id;
    mctx.bytes     = crd::containers::as_const_span(pbrm->cooked_bytes);
    mctx.manager   = nullptr;
    mctx.allocator = &alloc;
    void* mp = mloader.load(mctx);
    REQUIRE(mp != nullptr);
    auto* mat = static_cast<crd::resources::OpenPbrMaterial*>(mp);
    CHECK(mat->textures.base_color == txtr->id);
    CHECK(mat->params.base_color[0] == 0.5F);
    CHECK(mat->params.base_color[1] == 1.0F);
    CHECK(mat->params.base_color[2] == 0.25F);

    // 4. the cooked TXTR chain → the DEVICE, VERBATIM (sRGB format byte 3 → hardware decode-on-sample)
    crd::resources::CrdrFile tfile(&alloc);
    REQUIRE(crd::resources::crdr_read(crd::containers::as_const_span(txtr->cooked_bytes), tfile, &alloc)
            == crd::resources::CrdrError::Ok);
    const crd::resources::CrdrChunk* thead = crd::resources::crdr_find_chunk(tfile, crd::resources::kFourCC_HEAD);
    const crd::resources::CrdrChunk* tmip0 = crd::resources::crdr_find_chunk(tfile, crd::resources::kFourCC_MIP0);
    const crd::resources::CrdrChunk* tmip1 = crd::resources::crdr_find_chunk(tfile, crd::resources::kFourCC_MIP1);
    REQUIRE(thead != nullptr);
    REQUIRE(tmip0 != nullptr);
    REQUIRE(tmip1 != nullptr);
    CHECK(thead->payload[12] == 3U);  // RGBA8UnormSrgb — the slot decided the color space
    CHECK(tmip1->payload[0] == 188U); // the LINEAR-SPACE-filtered mip (the byte-box bug would be 127)
    const void* mips[2] = {tmip0->payload.data(), tmip1->payload.data()};
    auto        texture = r.raster->create_texture_from_mips(2U, 2U, 2U, mips, /*srgb=*/true);
    REQUIRE(texture != nullptr);

    // 5. RENDER through the gpu-context stack: CKIR FS texelFetches MIP 1 and modulates by the authored base_color
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_geo3_material_fetch_fs(fg, fe, mat->params.base_color[0], mat->params.base_color[1],
                                               mat->params.base_color[2]);
    auto vs = r.ctx->create_program(vg, ve);
    auto fs = r.ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = r.raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    constexpr crd::u32 dim    = 16U;
    auto               target = r.raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    r.raster->draw_textured(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *texture, 3U);

    // 6. the gate: cooked 188 → sRGB decode ≈ 0.503 → × (0.5, 1.0, 0.25) → ≈ (64, 128, 32). The re-derived-box
    //    counterfactual (127 → 0.216) would land at (28, 55, 14) — unconfusable.
    const crd::u32 px = target->read_pixel(dim / 2U, dim / 2U);
    const crd::u32 pr = px & 0xFFU;
    const crd::u32 pg = (px >> 8U) & 0xFFU;
    const crd::u32 pb = (px >> 16U) & 0xFFU;
    WARN("[geo3-close vulkan] rgb=(" << pr << "," << pg << "," << pb << ") expect ~(64,128,32)");
    CHECK(pr >= 60U);
    CHECK(pr <= 68U);
    CHECK(pg >= 124U);
    CHECK(pg <= 132U);
    CHECK(pb >= 29U);
    CHECK(pb <= 35U);

    mloader.unload(mp);
    (void)pfs::remove_file(pfs::Path(crd::containers::StringView(src_path)));
    (void)pfs::remove_file(pfs::Path(crd::containers::StringView("cerid_geo3_close.glb.tex.0_checker.meta")));
    (void)pfs::remove_file(pfs::Path(crd::containers::StringView("cerid_geo3_close.glb.mtl.0_tinted.meta")));
    (void)pfs::remove_file(pfs::Path(crd::containers::StringView("cerid_geo3_close.glb.scen.meta")));
}

// ═══ RET-2 (D-007 row 90, ADR-0105): the PRESENT surface — gpu-context drives a real swapchain ═════════════════════════
// The retirement capability crd-rhi held hostage: acquire → blit-the-canvas → present → resize/recreate, on a
// HEADLESS surface (VK_EXT_headless_surface — the FULL VkSwapchainKHR machinery with zero window system, so the gate
// runs anywhere). The canvas design: the app renders into a normal color target through the UNCHANGED draw paths and
// present() blits it into the backbuffer — present is a pure sink.

TEST_CASE("RET-2: gpu-context PRESENTS -- acquire/blit/present/resize through a real swapchain (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    namespace kir = crd::kir;

    // the WINDOWED context configuration — the exact one the sandbox ships on (headless contexts still work when the
    // loader offers VK_EXT_headless_surface; this Windows stack doesn't, so the gate runs the REAL-window path)
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = false;
    cfg.enable_validation = true; // RET-4: the present path must be validation-SILENT, asserted by counters
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk); // RET-4: the gpu-context capture — the whole gate runs under it
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (!vk->present_capable())
    {
        WARN("no present capability on this device; skipping");
        return;
    }
    crd::memory::TlsfAllocator alloc(4U << 20U);

    // a HEADLESS surface when the loader offers one; else a REAL Win32 window (the isolated helper TU).
    // 256×256 sits above the OS minimum window width, so the swapchain extent matches the request exactly.
    void* native = nullptr;
    if (!vk->headless_surface())
    {
        native = crd::gputest::create_test_window(256U, 256U);
        if (native == nullptr)
        {
            WARN("no VK_EXT_headless_surface and no platform window available; skipping");
            return;
        }
    }
    auto surface = raster->create_present_surface(native, 256U, 256U, gpu::PresentMode::Fifo);
    REQUIRE(surface != nullptr);
    CHECK(surface->valid());
    CHECK(surface->width() == 256U);
    CHECK(surface->height() == 256U);

    // the canvas: the CKIR triangle drawn through the normal offscreen path (present is a pure sink over it)
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);

    const crd::u32 sw     = surface->width(); // a real window's swapchain follows the WINDOW's client extent
    const crd::u32 sh     = surface->height();
    auto           target = raster->create_color_target(sw, sh);
    REQUIRE(target != nullptr);

    for (int frame = 0; frame < 3; ++frame) // three full acquire→blit→present cycles
    {
        raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
        CHECK(surface->present(*target));
        crd::gputest::pump_test_window();
    }
    CHECK(surface->frame_count() == 3U);

    // a mismatched canvas is REFUSED (never a stretched half-frame)
    auto small = raster->create_color_target(sw / 2U, sh / 2U);
    REQUIRE(small != nullptr);
    raster->draw(*small, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    CHECK(!surface->present(*small));

    // resize RECREATES the swapchain (oldSwapchain retire path) and the matching canvas presents again
    REQUIRE(surface->resize(sw, sh));
    raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
    CHECK(surface->present(*target));
    CHECK(surface->frame_count() == 4U);

    WARN("[ret2-present vulkan] 4 frames presented through a REAL swapchain ("
         << (native != nullptr ? "win32 window" : "headless surface") << ", " << sw << "x" << sh << ")");
    surface.reset(); // the surface dies BEFORE its window
    crd::gputest::destroy_test_window(native);

    // RET-4: the whole present lifecycle — creation, 4 presents, a REFUSED mismatch, resize/recreate, teardown —
    // was validation-SILENT (counters, never eyeballed logs; the ported crd::gpu::ValidationCapture)
    // (the S6 suballocation gate lives in the next TEST_CASE)
    if (capture.error_or_warning_count() > 0U) // diagnose on failure: the FIRST few captured messages, verbatim
    {
        const auto msgs  = capture.messages();
        crd::u32   shown = 0;
        for (crd::usize i = 0; i < msgs.size() && shown < 4U; ++i)
        {
            if (msgs[i].severity == crd::gpu::ValidationSeverity::Info) { continue; }
            WARN("[ret2 capture] id=" << msgs[i].message_id_number << " " << msgs[i].message_text.c_str());
            ++shown;
        }
    }
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// ═══ RET-4 pt 2 (D-007 row 92, ADR-0085 S6 absorbed): the SUBALLOCATOR — pooled device memory on gpu-context ═══════════
// The retired rhi allocator's S6 contract, re-proven on the one graphics layer: MANY small images share FEW
// VkDeviceMemory blocks (per-image vkAllocateMemory burned maxMemoryAllocationCount ≈ 4096 and driver time), and a
// full destroy → recreate cycle REUSES the pooled blocks (O(1) coalescing free, zero growth). Validation-SILENT.

TEST_CASE("RET-4: the absorbed S6 suballocator -- 48 small images share pooled blocks, freed space is REUSED (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk);
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    CHECK(gpu::vulkan_raster_block_count(*raster) == 0U); // no images yet — no blocks

    constexpr crd::u32 image_count = 48U;
    crd::u8            px[16U * 16U * 4U];
    for (crd::usize i = 0; i < sizeof(px); ++i) { px[i] = static_cast<crd::u8>(i); }

    crd::containers::Array<std::unique_ptr<gpu::ITexture>> textures(crd::memory::default_allocator());
    for (crd::u32 i = 0; i < image_count; ++i)
    {
        auto t = raster->create_texture(16U, 16U, px);
        REQUIRE(t != nullptr);
        textures.push_back(std::move(t));
    }
    const crd::u32 blocks_full = gpu::vulkan_raster_block_count(*raster);
    WARN("[ret4-suballoc vulkan] " << image_count << " images -> " << blocks_full << " pooled VkDeviceMemory block(s)");
    CHECK(blocks_full >= 1U);
    CHECK(blocks_full <= 3U); // 48 tiny images pool into a handful of blocks — never 48 driver allocations

    textures.clear(); // free all 48 → the pooled space coalesces (blocks stay, ready for reuse)
    for (crd::u32 i = 0; i < image_count; ++i)
    {
        auto t = raster->create_texture(16U, 16U, px);
        REQUIRE(t != nullptr);
        textures.push_back(std::move(t));
    }
    CHECK(gpu::vulkan_raster_block_count(*raster) == blocks_full); // REUSE, not growth — the O(1)-free proof

    textures.clear();
    CHECK(capture.error_count() == 0U); // the whole pool/free/reuse cycle validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

// ═══ RET-4 pt 3 (S7 compaction): DRAINED pools return their VkDeviceMemory to the driver ══════════════════════════════
TEST_CASE("RET-4: compact() releases drained blocks; indices stay STABLE across the tombstone (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk);
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    crd::u8 px[8U * 8U * 4U] = {};

    { // create → an IMAGE pool block + a LINEAR pool block (pt 4: even transient upload STAGING pools now)
        auto a = raster->create_texture(8U, 8U, px);
        auto b = raster->create_texture(8U, 8U, px);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(gpu::vulkan_raster_block_count(*raster) == 2U);
        // compact with the textures ALIVE: exactly the drained staging block releases; the live image block NEVER
        CHECK(gpu::vulkan_raster_compact(*raster) == 1U);
        CHECK(gpu::vulkan_raster_block_count(*raster) == 1U);
    } // both textures die → the image pool drains too

    CHECK(gpu::vulkan_raster_block_count(*raster) == 1U); // still held (reuse-ready)
    CHECK(gpu::vulkan_raster_compact(*raster) == 1U);     // the S7 verb: the drained block RETURNS to the driver
    CHECK(gpu::vulkan_raster_block_count(*raster) == 0U);

    // allocation after compaction: fresh blocks occupy the TOMBSTONE slots (index stability by design)
    auto keep = raster->create_texture(8U, 8U, px);
    REQUIRE(keep != nullptr);
    CHECK(gpu::vulkan_raster_block_count(*raster) == 2U); // image + staging pools again, in the reused slots

    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}

// ═══ RET-4 pt 5 (S7 relocation, absorbed): defrag RELOCATES live storage and PRESERVES contents ═══════════════════════
// The retired rhi S7 buffer-defrag contract, re-proven on gpu-context: punch holes in the pool (destroy half the
// buffers), relocate the survivors (recreate + GPU copy + swap inside the bundle), and every surviving byte is
// intact — verified through the NEW `download_storage` (upload_storage's twin). Validation-SILENT throughout.

TEST_CASE("RET-4: storage defrag relocates live buffers and PRESERVES contents (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk);
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    constexpr crd::u32 n      = 8U;
    constexpr crd::u32 words  = 256U; // 1 KiB per buffer
    std::unique_ptr<gpu::IStorageBuffer> bufs[n];
    crd::u32                             pattern[words];
    for (crd::u32 i = 0; i < n; ++i)
    {
        bufs[i] = raster->create_storage_buffer(words * 4U);
        REQUIRE(bufs[i] != nullptr);
        for (crd::u32 w = 0; w < words; ++w) { pattern[w] = (i << 16U) | w; } // a per-buffer, per-word signature
        REQUIRE(raster->upload_storage(*bufs[i], 0U, pattern, words * 4U));
    }

    for (crd::u32 i = 0; i < n; i += 2U) { bufs[i].reset(); } // punch holes: destroy the even-indexed buffers

    const crd::u32 relocations = gpu::vulkan_raster_defragment(*raster);
    CHECK(relocations == n / 2U); // every SURVIVOR moved (recreate + copy + swap)

    for (crd::u32 i = 1U; i < n; i += 2U) // every surviving byte is intact at the relocated address
    {
        REQUIRE(raster->download_storage(*bufs[i]));
        CHECK(bufs[i]->read_u32(0U) == ((i << 16U) | 0U));
        CHECK(bufs[i]->read_u32(words - 1U) == ((i << 16U) | (words - 1U)));
        CHECK(bufs[i]->read_u32(words / 2U) == ((i << 16U) | (words / 2U)));
    }

    // ── the IMAGE half of the S7 contract: a mipped texture relocates and still SAMPLES correctly ─────────────────
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(4U << 20U);
    constexpr crd::u32         tw = 16U;
    crd::u8                    tex_px[tw * tw * 4U];
    crd::gputest::fill_left_red_right_green(tex_px, tw, tw);
    auto texture = raster->create_texture_mipped(tw, tw, tex_px); // TRANSFER_SRC-capable ⇒ relocatable
    REQUIRE(texture != nullptr);

    const crd::u32 img_relocations = gpu::vulkan_raster_defragment(*raster);
    CHECK(img_relocations >= n / 2U + 1U); // the storage buffers again + AT LEAST the texture moved

    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_textured_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_sample_fs(fg, fe);
    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto program = raster->create_raster_program(*vs, *fs);
    REQUIRE(program != nullptr);
    auto target = raster->create_color_target(32U, 32U);
    REQUIRE(target != nullptr);
    raster->draw_textured(*target, *program, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, *texture, 3U);
    const crd::u32 left  = target->read_pixel(8U, 16U);  // the RELOCATED texture samples exactly as before the move
    const crd::u32 right = target->read_pixel(24U, 16U);
    CHECK((left & 0xFFU) > 200U);          // left: red
    CHECK(((right >> 8U) & 0xFFU) > 200U); // right: green

    CHECK(capture.error_count() == 0U); // barriers + copies + swaps all validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

// ═══ RET-5 (D-007 row 93, ADR-0105): ImGui renders through the gpu-context OVERLAY present ════════════════════════════
// The crd-imgui GPU backend (ImGuiGpuBackend — app-free, window-free) initializes against the context's handles +
// the surface's swapchain parameters and records its draw data into the present overlay pass: scene canvas blitted,
// ImGui composited onto the backbuffer, presented — the full HUD path on the ONE graphics layer, validation-SILENT.

TEST_CASE("RET-5: ImGui composites through the gpu-context overlay present (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    namespace kir = crd::kir;
    gpu::GpuContextConfig cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = false;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk);
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    if (!vk->present_capable()) { WARN("no present capability; skipping"); return; }

    void* native = nullptr;
    if (!vk->headless_surface())
    {
        native = crd::gputest::create_test_window(256U, 256U);
        if (native == nullptr) { WARN("no platform window available; skipping"); return; }
    }
    auto surface = raster->create_present_surface(native, 256U, 256U, gpu::PresentMode::Fifo);
    REQUIRE(surface != nullptr);

    // a WINDOWLESS ImGui context (no GLFW — the platform-input layer is a separate concern; IO driven by hand)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().DisplaySize = ImVec2(256.0F, 256.0F);
    {
        crd::imgui::ImGuiGpuBackend backend(*vk, *surface);
        REQUIRE(backend.valid());

        crd::memory::TlsfAllocator alloc(4U << 20U);
        kir::KGraph                vg(&alloc);
        kir::KEntry                ve;
        crd::gputest::build_triangle_vs(vg, ve);
        kir::KGraph fg(&alloc);
        kir::KEntry fe;
        crd::gputest::build_triangle_fs(fg, fe);
        auto vs = ctx->create_program(vg, ve);
        auto fs = ctx->create_program(fg, fe);
        REQUIRE(vs != nullptr);
        REQUIRE(fs != nullptr);
        auto program = raster->create_raster_program(*vs, *fs);
        REQUIRE(program != nullptr);
        auto target = raster->create_color_target(surface->width(), surface->height());
        REQUIRE(target != nullptr);

        for (int frame = 0; frame < 3; ++frame) // scene → blit → ImGui overlay → present, three full frames
        {
            raster->draw(*target, *program, gpu::ClearColor{0.0F, 0.0F, 0.0F, 1.0F}, 3U);
            backend.new_frame();
            ImGui::NewFrame();
            ImGui::Begin("cerid");
            ImGui::Text("gpu-context overlay frame %d", frame);
            ImGui::End();
            ImGui::Render();
            CHECK(surface->present(*target, &crd::imgui::ImGuiGpuBackend::overlay_thunk, &backend));
            crd::gputest::pump_test_window();
        }
        CHECK(surface->frame_count() == 3U);
    } // the backend drains + shuts down BEFORE the ImGui context dies
    ImGui::DestroyContext();

    CHECK(capture.error_count() == 0U); // init + font upload + 3 composited frames + teardown, all SILENT
    CHECK(capture.warning_count() == 0U);
    surface.reset(); // the surface dies BEFORE its window
    crd::gputest::destroy_test_window(native);
}

// ---------------------------------------------------------------------------------------------------------------------
// RET-6 (ADR-0105): the OVERLAY DRAW -- crd-draw's CKIR line shader composites ONTO an existing scene through the NEW
// draw_overlay seam: color loadOp=LOAD (the scene STAYS -- never cleared), standard alpha blending, vertex-pulled
// instances from the u32 draw buffer (storage_load + intBitsToFloat recovery -- the GEO-1 idiom). The gate proves all
// three composition properties by pixel: ON the line = the line's white; OFF the line inside the scene's triangle =
// still red (the LOAD proof); outside both = the clear blue (untouched). Validation-SILENT by counter.
TEST_CASE("RET-6: draw_overlay composites the CKIR line shader over an existing scene (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    gpu::GpuContextConfig      cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk);
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // 1. the SCENE: the standard CKIR triangle (red) into a 64x64 target
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto scene = raster->create_raster_program(*vs, *fs);
    REQUIRE(scene != nullptr);
    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *scene, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // 2. the OVERLAY program — ⭐⭐ REN-38-F7: cooked from the AUTHORED declarations (`draw_assets.hpp`), the
    // same texts crd-draw's init cooks. The hand-written `ckir_draw.hpp` builders this gate used to exercise
    // are DELETED; this pixel gate is the proof the authored suite renders identically.
    kir::KGraph lvg(&alloc);
    kir::KEntry lve;
    {
        crd::vertcook::VertexProgramDesc d(&alloc);
        crd::containers::String          where(&alloc);
        REQUIRE(crd::vertcook::parse_vertex_toml(crd::containers::StringView(crd::draw::kDrawLineVs), d, &where)
                == crd::vertcook::VertexCookError::Ok);
        REQUIRE(crd::vertcook::cook_vertex_program(d, lvg, lve));
    }
    kir::KGraph lfg(&alloc);
    kir::KEntry lfe;
    {
        namespace ck = crd::kir::cook;
        namespace tq = crd::kir::technique;
        crd::matcook::MaterialDesc d(&alloc);
        crd::containers::String    where(&alloc);
        REQUIRE(crd::matcook::parse_material_toml(crd::containers::StringView(crd::draw::kDrawLineMat), d, &where)
                == crd::matcook::MaterialCookError::Ok);
        const auto kf = [&](double v) { return lfg.constant(v, kir::make_shape({1}), kir::DType::F32); };
        ck::SurfaceInputs in;
        in.world_normal = lfg.vec3(kf(0.0), kf(0.0), kf(1.0));
        in.world_pos    = lfg.vec3(kf(0.0), kf(0.0), kf(0.0));
        in.view_dir     = lfg.vec3(kf(0.0), kf(0.0), kf(1.0));
        const auto surface_thunk = [](kir::KGraph& gg, int sid, const ck::SurfaceInputs&, void* user) {
            return crd::matcook::cook_material(*static_cast<const crd::matcook::MaterialDesc*>(user), gg, sid);
        };
        const ck::MaterialTemplate tmpl{surface_thunk, &d};
        const ck::VariantOptions   opts{crd::kir::material::AlphaMode::Opaque, 0.5};
        const tq::Technique        un = tq::unlit();
        REQUIRE(tq::build_fs_for_pass(tmpl, un, ck::PassType::Forward, opts, in, lfg, lfe,
                                      lfg.vec3(kf(0.0), kf(0.0), kf(1.0)), lfg.vec3(kf(1.0), kf(1.0), kf(1.0)),
                                      nullptr, 0, nullptr, 0));
    }
    auto lvs = ctx->create_program(lvg, lve);
    auto lfs = ctx->create_program(lfg, lfe);
    REQUIRE(lvs != nullptr); // the u32 record pull + intBitsToFloat lowers through the VERTEX stage
    REQUIRE(lfs != nullptr);
    auto line = raster->create_raster_program(*lvs, *lfs);
    REQUIRE(line != nullptr);

    // 3. the DRAW BUFFER: the 32-word header (identity view_proj, 64x64 viewport, all categories on) + ONE white
    //    horizontal line through NDC y=0 (pixel row 32), width 8 px
    crd::containers::Array<crd::u32> words(&alloc);
    const auto fbits = [](float f) { crd::u32 u = 0; std::memcpy(&u, &f, 4U); return u; };
    for (crd::u32 c = 0; c < 4U; ++c)
    {
        for (crd::u32 rr = 0; rr < 4U; ++rr) { words.push_back(fbits(c == rr ? 1.0F : 0.0F)); } // identity, column-major
    }
    words.push_back(fbits(64.0F));      // [16] viewport_px.x
    words.push_back(fbits(64.0F));      // [17] viewport_px.y
    words.push_back(0xFFFFFFFFU);       // [18] category_mask -- all on
    words.push_back(fbits(0.0F));       // [19] time_s
    while (words.size() < 32U) { words.push_back(0U); } // grid words unused by the line shaders
    const float line_inst[9] = {-2.0F, 0.0F, 0.0F, 2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 8.0F};
    for (crd::u32 wi = 0; wi < 9U; ++wi)
    {
        if (wi == 6U) { words.push_back(0xFFFFFFFFU); }      // color: opaque white
        else if (wi == 7U) { words.push_back(0U); }          // flags: category 0, Depth::Always
        else { words.push_back(fbits(line_inst[wi])); }
    }
    auto storage = raster->create_storage_buffer(static_cast<crd::u32>(words.size() * 4U));
    REQUIRE(storage != nullptr);
    REQUIRE(raster->upload_storage(*storage, 0U, words.data(), static_cast<crd::u32>(words.size() * 4U)));

    // 4. COMPOSITE: 6 vertices = 1 line instance, no depth (Always)
    REQUIRE(raster->draw_overlay(*target, *line, *storage, gpu::DepthCompare::Always, 6U));

    // 5. the pixel gate
    const crd::u32 on_line  = target->read_pixel(32U, 32U); // the line's core -- white over the red triangle
    const crd::u32 off_line = target->read_pixel(32U, 48U); // inside the triangle, 16 px below the line -- still red
    const crd::u32 corner   = target->read_pixel(2U, 2U);   // outside everything -- the clear blue survives the LOAD
    const auto     rch      = [](crd::u32 p) { return p & 0xFFU; };
    const auto     gch      = [](crd::u32 p) { return (p >> 8U) & 0xFFU; };
    const auto     bch      = [](crd::u32 p) { return (p >> 16U) & 0xFFU; };
    WARN("[ret6-overlay vulkan] on=(" << rch(on_line) << "," << gch(on_line) << "," << bch(on_line) << ") off=("
                                      << rch(off_line) << "," << gch(off_line) << "," << bch(off_line) << ") corner=("
                                      << rch(corner) << "," << gch(corner) << "," << bch(corner) << ")");
    CHECK(rch(on_line) > 200U);  // white line: all channels high
    CHECK(gch(on_line) > 200U);
    CHECK(bch(on_line) > 200U);
    CHECK(rch(off_line) > 200U); // the scene's red SURVIVED the overlay (loadOp=LOAD, not clear)
    CHECK(gch(off_line) < 50U);
    CHECK(rch(corner) < 50U);    // untouched clear blue
    CHECK(bch(corner) > 200U);

    if (capture.error_or_warning_count() > 0U) // diagnose on failure: the FIRST few captured messages, verbatim
    {
        const auto msgs  = capture.messages();
        crd::u32   shown = 0;
        for (crd::usize mi = 0; mi < msgs.size() && shown < 4U; ++mi)
        {
            if (msgs[mi].severity == crd::gpu::ValidationSeverity::Info) { continue; }
            WARN("[ret6 capture] id=" << msgs[mi].message_id_number << " " << msgs[mi].message_text.c_str());
            ++shown;
        }
    }
    CHECK(capture.error_count() == 0U); // the whole overlay lifecycle is validation-SILENT
    CHECK(capture.warning_count() == 0U);
}

// ---------------------------------------------------------------------------------------------------------------------
// RET-6 pt 3: the crd-draw GPU HALF on gpu-context -- init compiles the CKIR suite through create_program (no
// ResourceManager, no cooked-GLSL pack, no pipelines), submit_overlay packs the RenderBuffer into the u32 draw buffer
// and composes it over the scene through the draw_overlay chain. The same pixel triple as the seam gate -- on-line
// white / off-line red (LOAD) / corner blue -- now through the FULL crd-draw path, validation-SILENT, clean shutdown.
TEST_CASE("RET-6: crd-draw init + submit_overlay compose a RenderBuffer over the scene (Vulkan)",
          "[gpu-context][vulkan][gpu][raster][ret]")
{
    namespace kir = crd::kir;
    crd::memory::TlsfAllocator alloc(16U << 20U);
    gpu::GpuContextConfig      cfg;
    cfg.backend           = gpu::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto ctx              = gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr) { WARN("no Vulkan device; skipping"); return; }
    auto* vk = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vk->shader_object()) { WARN("no VK_EXT_shader_object; skipping"); return; }
    crd::gpu::ValidationCapture capture(*vk);
    auto raster = gpu::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);

    // the scene: the CKIR triangle (red) over a blue clear
    kir::KGraph vg(&alloc);
    kir::KEntry ve;
    crd::gputest::build_triangle_vs(vg, ve);
    kir::KGraph fg(&alloc);
    kir::KEntry fe;
    crd::gputest::build_triangle_fs(fg, fe);
    auto vs = ctx->create_program(vg, ve);
    auto fs = ctx->create_program(fg, fe);
    REQUIRE(vs != nullptr);
    REQUIRE(fs != nullptr);
    auto scene = raster->create_raster_program(*vs, *fs);
    REQUIRE(scene != nullptr);
    constexpr crd::u32 dim    = 64U;
    auto               target = raster->create_color_target(dim, dim);
    REQUIRE(target != nullptr);
    raster->draw(*target, *scene, gpu::ClearColor{0.0F, 0.0F, 1.0F, 1.0F}, 3U);

    // the crd-draw GPU half: CKIR programs + the draw buffer, no pack, no resources
    REQUIRE(crd::draw::init(*vk, *raster));
    REQUIRE(crd::draw::is_initialised());

    crd::draw::RenderBuffer buf(crd::memory::default_allocator());
    crd::draw::DebugLine    line;
    line.a     = {-2.0F, 0.0F, 0.0F};
    line.b     = {2.0F, 0.0F, 0.0F};
    line.color = 0xFFFFFFFFU; // opaque white
    line.width = 8.0F;
    buf.add_line(line);

    crd::draw::OverlayPassConfig ocfg;
    ocfg.view_proj   = crd::math::Mat4f{1.0F}; // identity: world == NDC
    ocfg.viewport_px = {static_cast<crd::f32>(dim), static_cast<crd::f32>(dim)};
    REQUIRE(crd::draw::submit_overlay(*target, buf, ocfg));

    const crd::u32 on_line  = target->read_pixel(32U, 32U);
    const crd::u32 off_line = target->read_pixel(32U, 48U);
    const crd::u32 corner   = target->read_pixel(2U, 2U);
    WARN("[ret6-drawhalf vulkan] on=(" << (on_line & 0xFFU) << "," << ((on_line >> 8U) & 0xFFU) << ","
                                       << ((on_line >> 16U) & 0xFFU) << ") off_r=" << (off_line & 0xFFU)
                                       << " corner_b=" << ((corner >> 16U) & 0xFFU));
    CHECK((on_line & 0xFFU) > 200U);            // white line core
    CHECK(((on_line >> 8U) & 0xFFU) > 200U);
    CHECK(((on_line >> 16U) & 0xFFU) > 200U);
    CHECK((off_line & 0xFFU) > 200U);           // the scene's red survived
    CHECK(((off_line >> 8U) & 0xFFU) < 50U);
    CHECK(((corner >> 16U) & 0xFFU) > 200U);    // untouched clear blue

    crd::draw::shutdown();
    CHECK_FALSE(crd::draw::is_initialised());

    if (capture.error_or_warning_count() > 0U)
    {
        const auto msgs  = capture.messages();
        crd::u32   shown = 0;
        for (crd::usize mi = 0; mi < msgs.size() && shown < 4U; ++mi)
        {
            if (msgs[mi].severity == crd::gpu::ValidationSeverity::Info) { continue; }
            WARN("[ret6-drawhalf capture] id=" << msgs[mi].message_id_number << " " << msgs[mi].message_text.c_str());
            ++shown;
        }
    }
    CHECK(capture.error_count() == 0U);
    CHECK(capture.warning_count() == 0U);
}
