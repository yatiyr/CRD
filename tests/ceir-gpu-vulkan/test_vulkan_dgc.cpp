// test_vulkan_dgc.cpp — CEIR-20c-2 DEVICE GATE: the authored ceir.work smoke runs as a VK_EXT_device_generated_commands
// execution — the CROSS-VENDOR third lowering of one ceir.work program. The AUTHORED work_smoke_produce_dgc.ckir writes BOTH the
// SEQUENCE COUNT (=5) and FIVE (1,1,1) dispatch payloads into device buffers; vkCmdExecuteGeneratedCommandsEXT then GENERATES
// five separate DISPATCH commands of the authored work_smoke_consume.ckir. out[0]==5 proves five DEVICE-GENERATED commands ran,
// and the count buffer doubles as the consume's queue read (out[1]==count==5). Soft-skips without the EXT cap.
//
// ⛔ SCOREBOARD (honest, verbatim from the band tracker): the ONE ceir.work program has THREE lowerings —
//   20b   = host-recorded dispatch, DEVICE-supplied grid (vkCmdDispatchIndirect: the host records ONE dispatch, the GPU sizes it)
//   20c-1 = GPU-SCHEDULED node launch (D3D12 Work Graphs: the producer emits a grid-launch record, the GPU self-schedules)
//   20c-2 = DEVICE-GENERATED command STREAM (THIS: the produce writes the COUNT + every dispatch payload; the host records only
//           vkCmdExecuteGeneratedCommandsEXT) — the thing a single dispatch-indirect literally cannot express (N distinct commands).
//
// ⭐ MODULE-DRIVEN: the produce->consume topology is derived from the SAME WorkBuildDesc via build_work_graph_plan (the work-topology
// plan, not a Work-Graphs-only artifact — 20z may rename it), so this gate is literally "same authored desc, THIRD execution".
// ⛔ Mandate #1: the two .ckir kernels are the SOLE source; the rig only cooks (emit GLSL -> SPIR-V) + dispatches them.

#include <crd/ceir/gpu/work_build.hpp> // WorkBuildDesc / WorkStageKind
#include <crd/ceir/gpu/work_graph.hpp> // build_work_graph_plan (module-driven produce->consume topology)
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_dgc_context.hpp>     // the VK_EXT_DGC offline rig
#include <crd/gpu/vulkan_shader_compile.hpp>  // compile_glsl_to_spirv
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read
#include <crd/kir/ckir_glsl.hpp>  // emit_compute_kernel_glsl
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace ceg = crd::ceir::gpu;
namespace gpu = crd::gpu;
namespace kir = crd::kir;

namespace
{
// Load a .ckir kernel → emit GLSL → SPIR-V (the raw bytes the rig builds a VkShaderModule from). Validates the authored format.
bool load_spirv(const char* path, const char* name, crd::containers::Array<crd::u8>& out, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) { return false; }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) { return false; }
    f.seekg(0);
    crd::containers::Array<char> src(a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph g(a);
    kir::KEntry e;
    if (!kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok)
    {
        std::fprintf(stderr, "[dgc] ckir_read FAILED for %s\n", name);
        return false;
    }
    kir::GlslKernel kern(a);
    if (!kir::emit_compute_kernel_glsl(g, e, a, kern))
    {
        std::fprintf(stderr, "[dgc] emit_compute_kernel_glsl FAILED for %s\n", name);
        return false;
    }
    const auto cres = gpu::compile_glsl_to_spirv(gpu::ShaderStage::Compute, crd::containers::to_view(kern.source), name, a);
    if (!cres.ok)
    {
        std::fprintf(stderr, "[dgc] compile_glsl_to_spirv FAILED for %s:\n%s\nGLSL:\n%s\n", name, cres.error_message.c_str(),
                     kern.source.data());
        return false;
    }
    out.resize(cres.spirv.size(), 0U);
    if (cres.spirv.size() > 0U) { std::memcpy(out.data(), cres.spirv.data(), cres.spirv.size()); }
    return true;
}
} // namespace

TEST_CASE("CEIR-20c-2: the authored ceir.work smoke runs as a VK_EXT_device_generated_commands stream -- five DEVICE-GENERATED "
          "dispatches, out[0]==5",
          "[ceir][ceir-gpu][vulkan][gpu][work][ceir20c]")
{
    gpu::GpuContextConfig gcfg{};
    gcfg.backend  = gpu::GpuBackend::Vulkan;
    gcfg.headless = true;
    auto ctx      = gpu::create_vulkan_gpu_context(gcfg);
    if (ctx == nullptr) { WARN("no Vulkan device available; skipping"); return; }
    auto* vkctx = static_cast<gpu::VulkanGpuContext*>(ctx.get());
    if (!vkctx->device_generated_commands_ext()) { WARN("no VK_EXT_device_generated_commands (cross-vendor) adapter; skipping"); return; }

    gpu::VulkanDgcContext dgc(*vkctx);
    REQUIRE(dgc.valid());
    crd::memory::TlsfAllocator alloc(16U << 20U);

    // (a) cook the two AUTHORED .ckir kernels to SPIR-V (validates the .ckir format as a side effect).
    crd::containers::Array<crd::u8> prod_spv(&alloc);
    crd::containers::Array<crd::u8> cons_spv(&alloc);
    REQUIRE(load_spirv(CRD_REPO_DIR "/assets/ckir/work_smoke_produce_dgc.ckir", "work_smoke_produce_dgc", prod_spv, &alloc));
    REQUIRE(load_spirv(CRD_REPO_DIR "/assets/ckir/work_smoke_consume.ckir", "work_smoke_consume", cons_spv, &alloc));

    // (b) MODULE-DRIVEN topology: the SAME WorkBuildDesc shape the other lowerings consume (one queue, produce writes it, consume
    // reads it) → build_work_graph_plan derives produce=entry, consume=downstream. This gate is "same desc, third execution".
    ceg::WorkBuildDesc desc{};
    desc.num_queues              = 1U;
    desc.queues[0].capacity      = 64U;
    desc.queues[0].record_stride = 4U;
    desc.queues[0].source_param  = 0xD6CU;
    desc.num_stages              = 2U;
    desc.stages[0].kind          = ceg::WorkStageKind::Produce;
    desc.stages[0].kernel        = crd::containers::StringView("work_smoke_produce_dgc");
    desc.stages[0].queue         = 0U;
    desc.stages[1].kind          = ceg::WorkStageKind::Consume;
    desc.stages[1].kernel        = crd::containers::StringView("work_smoke_consume");
    desc.stages[1].queue         = 0U;
    ceg::WorkGraphPlan plan{};
    REQUIRE(ceg::build_work_graph_plan(desc, plan));
    REQUIRE(plan.num_nodes == 2U);
    REQUIRE(plan.entry != ceg::kNoWorkGraphNode);
    REQUIRE(plan.nodes[plan.entry].role == ceg::WorkStageKind::Produce);
    // ⭐ resolve the produce (entry) + consume (its downstream) NODES + their SPIR-V FROM THE PLAN's kernel symbols — the
    // topology the module derived selects which cooked kernel plays each role (not the test). This is what makes "same desc,
    // third execution" TRUE rather than decorative (the 20c-1 is_prod resolution mirrored for the DGC rig).
    const crd::u32 entry_node = plan.entry;
    const crd::u32 down_node   = plan.nodes[entry_node].downstream;
    REQUIRE(down_node != ceg::kNoWorkGraphNode);
    REQUIRE(plan.nodes[down_node].role == ceg::WorkStageKind::Consume);
    const auto spirv_for = [&](crd::containers::StringView kernel) -> crd::containers::ConstSpan<crd::u8> {
        if (kernel == crd::containers::StringView("work_smoke_produce_dgc"))
        {
            return crd::containers::ConstSpan<crd::u8>(prod_spv.data(), prod_spv.size());
        }
        return crd::containers::ConstSpan<crd::u8>(cons_spv.data(), cons_spv.size());
    };

    // (c) the three global buffers of the smoke's DGC topology:
    //   [0] token = five VkDispatchIndirectCommand payloads (15 u32) — the command STREAM (indirectAddress); produce writes it.
    //   [1] count = the device SEQUENCE COUNT (=5, sequenceCountAddress); produce writes it; ALSO the consume's queue@0 read source.
    //   [2] out   = [atomic invocation counter, count readback]; the consume writes it.
    constexpr crd::u32 n_seq = 5U;
    crd::u32 token_rb[15] = {};
    crd::u32 count_rb[1]  = {};
    crd::u32 out_rb[2]    = {};
    gpu::VulkanDgcContext::Buffer buffers[3] = {
        {nullptr, token_rb, sizeof(token_rb)}, // token (produce writes; readback checks token[0]==1)
        {nullptr, count_rb, sizeof(count_rb)}, // count (produce writes 5; readback checks ==5)
        {nullptr, out_rb, sizeof(out_rb)},     // out   (consume writes; zero-initialised for the atomic counter)
    };
    // bind maps (std430 binding N == the .ckir BufferDecl iidx): produce {token@0, count@1}; consume {count-as-queue@0, out@1}.
    const crd::u32 produce_binds[2] = {0U, 1U};
    const crd::u32 consume_binds[2] = {1U, 2U};

    gpu::VulkanDgcContext::ExecuteDesc ed{};
    ed.produce_spirv   = spirv_for(plan.nodes[entry_node].kernel); // the plan's ENTRY kernel plays produce
    ed.consume_spirv   = spirv_for(plan.nodes[down_node].kernel);  // its DOWNSTREAM kernel plays consume
    ed.produce_binds   = produce_binds;
    ed.n_produce_binds = 2U;
    ed.consume_binds   = consume_binds;
    ed.n_consume_binds = 2U;
    ed.token_buffer    = 0U;
    ed.count_buffer    = 1U;
    ed.max_seq         = n_seq;
    ed.dispatch_stride = 12U; // sizeof(VkDispatchIndirectCommand)

    REQUIRE(dgc.dispatch_generated(ed, crd::containers::ConstSpan<gpu::VulkanDgcContext::Buffer>(buffers, 3U)));

    CHECK(token_rb[0] == 1U); // the produce authored the command payloads (each dispatch grid = (1,1,1))
    CHECK(count_rb[0] == n_seq); // the produce authored the DEVICE sequence count
    CHECK(out_rb[0] == n_seq);   // ⭐ FIVE device-generated dispatch commands each ran one invocation — out[0] == the device count
    CHECK(out_rb[1] == n_seq);   // each consume invocation read the count buffer (bound as queue@0) == 5
}
