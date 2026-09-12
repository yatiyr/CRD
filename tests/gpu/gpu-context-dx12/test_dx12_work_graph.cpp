// test_dx12_work_graph.cpp — CEIR-20c-1 DEVICE GATE: the AUTHORED ceir.work smoke runs as a D3D12 WORK GRAPH. The two
// committed .ckir kernels (work_smoke_produce/consume) are cooked into a Work Graph node LIBRARY
// (emit_work_graph_library_hlsl), compiled to DXIL (lib_6_8), and DispatchGraph'd: the producer node writes the (count,1,1)
// header + emits a grid-launch RECORD carrying that count; the GPU SELF-SCHEDULES the consumer node SIZED by the record —
// so out[0] == the device count with NO host submit boundary between the stages (the 20c gold standard vs 20b's host-read
// fallback). Mandate #1: the .ckir is the SOLE source; the node ABI is cook-emitted. Soft-skips without a WorkGraphsTier 1.0
// adapter. ⛔ SCOREBOARD: this proves GPU-SCHEDULED launch via grid-launch records; record-PAYLOAD flow is work.record's
// future slice (ledgered).

#include <crd/gpu/dx12_context.hpp>           // compile_work_graph_library_to_dxil
#include <crd/gpu/dx12_work_graph_context.hpp> // the Work Graphs offline rig

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_read
#include <crd/kir/ckir_hlsl.hpp>  // emit_work_graph_library_hlsl

#include <crd/ceir/gpu/work_build.hpp> // CEIR-20c-1c: WorkBuildDesc (the authored ceir.work program)
#include <crd/ceir/gpu/work_graph.hpp> // CEIR-20c-1c: build_work_graph_plan (derive the Work Graph topology)

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <fstream>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "."
#endif

namespace gpu = crd::gpu;
namespace kir = crd::kir;
namespace ceg = crd::ceir::gpu;

namespace
{
bool load_ckir(const char* path, kir::KGraph& g, kir::KEntry& e, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.good()) { return false; }
    const std::streamsize sz = f.tellg();
    if (sz <= 0) { return false; }
    f.seekg(0);
    crd::containers::Array<char> src(a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    return kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok;
}
} // namespace

TEST_CASE("CEIR-20c-1: the authored ceir.work smoke runs as a D3D12 WORK GRAPH -- GPU-scheduled produce->consume, out[0]==count",
          "[gpu-context][dx12][gpu][work][ceir20c]")
{
    gpu::Dx12WorkGraphContext wg;
    if (!wg.valid()) { WARN("no D3D12 Work Graphs (WorkGraphsTier 1.0) adapter; skipping"); return; }
    crd::memory::TlsfAllocator alloc(16U << 20U);

    kir::KGraph pg(&alloc);
    kir::KGraph cg(&alloc);
    kir::KEntry pe;
    kir::KEntry ce;
    REQUIRE(load_ckir(CRD_REPO_DIR "/assets/ckir/work_smoke_produce.ckir", pg, pe, &alloc));
    REQUIRE(load_ckir(CRD_REPO_DIR "/assets/ckir/work_smoke_consume.ckir", cg, ce, &alloc));

    // ⭐ MODULE-DRIVEN: the SAME authored ceir.work WorkBuildDesc the 20b compute-indirect fallback consumes (one queue,
    // produce writes it, consume reads it). build_work_graph_plan derives the produce->consume TOPOLOGY; the gate resolves
    // each node's kernel symbol to its .ckir and emits the node library FROM the plan (not a hardcoded graph).
    ceg::WorkBuildDesc desc;
    desc.num_queues              = 1U;
    desc.queues[0].capacity      = 64U;
    desc.queues[0].record_stride = 4U;
    desc.queues[0].source_param  = 0xC0FFEEU;
    desc.num_stages              = 2U;
    desc.stages[0].kind          = ceg::WorkStageKind::Produce;
    desc.stages[0].kernel        = crd::containers::StringView("work_smoke_produce");
    desc.stages[0].queue         = 0U;
    desc.stages[1].kind          = ceg::WorkStageKind::Consume;
    desc.stages[1].kernel        = crd::containers::StringView("work_smoke_consume");
    desc.stages[1].queue         = 0U;
    ceg::WorkGraphPlan plan;
    REQUIRE(ceg::build_work_graph_plan(desc, plan));
    REQUIRE(plan.num_nodes == 2U);

    // resolve each plan node's kernel -> its loaded .ckir, and emit the node descs FROM the plan topology (produce entry +
    // its downstream consumer). The smoke's queue is buffer slot 0 in both kernels.
    kir::WorkGraphNodeDesc nodes[8];
    for (crd::u32 i = 0; i < plan.num_nodes; ++i)
    {
        const bool is_prod   = plan.nodes[i].kernel == crd::containers::StringView("work_smoke_produce");
        nodes[i].g           = is_prod ? &pg : &cg;
        nodes[i].entry       = is_prod ? &pe : &ce;
        nodes[i].kind        = plan.nodes[i].role == ceg::WorkStageKind::Produce ? kir::WorkGraphNodeKind::Produce
                                                                                 : kir::WorkGraphNodeKind::Consume;
        nodes[i].queue_iidx  = 0U;
        nodes[i].id          = plan.nodes[i].kernel; // the HLSL node function name == the kernel symbol
        nodes[i].consumer_id = (plan.nodes[i].downstream != ceg::kNoWorkGraphNode)
                                   ? plan.nodes[plan.nodes[i].downstream].kernel
                                   : crd::containers::StringView("");
        nodes[i].max_grid    = 64U;
    }
    kir::GlslKernel lib(&alloc);
    REQUIRE(kir::emit_work_graph_library_hlsl(nodes, plan.num_nodes, &alloc, lib));

    const auto dxil = gpu::compile_work_graph_library_to_dxil(crd::containers::to_view(lib.source),
                                                              crd::containers::StringView("wg_lib"), &alloc);
    INFO(dxil.error_message.c_str());
    REQUIRE(dxil.ok);

    // buffers: queue (reg 0 — the (count,1,1) header + record slots) + out (reg 1 — out[0]=atomic counter, out[1]=count).
    constexpr crd::u32 kN         = 5U;
    crd::u32           queue_rb[19] = {}; // (count,1,1) header + 16 record slots
    crd::u32           out_rb[2]    = {};
    const gpu::Dx12WorkGraphContext::Binding binds[2] = {{nullptr, queue_rb, sizeof(queue_rb), 0U},
                                                         {nullptr, out_rb, sizeof(out_rb), 1U}};
    REQUIRE(wg.dispatch_graph(crd::containers::ConstSpan<crd::u8>(dxil.dxil.data(), dxil.dxil.size()), "wg_lib",
                              crd::containers::ConstSpan<gpu::Dx12WorkGraphContext::Binding>(binds, 2U)));

    CHECK(queue_rb[0] == kN); // the producer wrote the device count header
    CHECK(out_rb[0] == kN);   // ⭐ the consumer ran count invocations -- the GPU sized it from the launch record, no host round-trip
    CHECK(out_rb[1] == kN);   // out[1] = queue[0], the count each invocation read back
}
