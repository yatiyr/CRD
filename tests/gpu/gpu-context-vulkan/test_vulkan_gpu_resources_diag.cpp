// The gpu.resources diagnostic command over a real Vulkan context and frame graph.
//
// The host registers a real context and a real frame graph with the command; the answers are checked against what the
// device side reports on its own: the identity registry's live count read directly (and moving by exactly one for one
// storage buffer), the context's own adapter name and validation activation, and the frame graph's own counters,
// including the aliasing relation two disjoint equal transients must show (physical bytes are half the logical bytes).
// Responses are parsed with the JSON reader. Device-gated: skips without a graphics-capable Vulkan device with shader
// objects (the hosted Windows lanes); runs on lavapipe and on a real GPU.

#include <crd/assetio/json.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/gpu/raster_context.hpp>
#include <crd/gpu/validation.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/gpu/vulkan_raster_context.hpp>
#include <crd/gpu/vulkan_validation_capture.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/gpu/gpu_resources_diag.hpp>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace g    = crd::gpu;
namespace json = crd::assetio::json;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc{crd::usize{16} << 20U, nullptr, "vk-gpu-resources-diag"};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void record_nothing(g::IFrameContext& /*ctx*/, void* /*user*/) {}

struct Answer
{
    json::JsonDoc doc{&g_alloc};
    crd::u32      summary = json::kInvalid;
    crd::u32      items   = json::kInvalid;
};

void ask(crd::perf::DiagCommandService& service, Answer& a)
{
    crd::perf::DiagRequest r;
    r.command    = crd::perf::gpu::kGpuResourcesCommand;
    r.page_items = crd::perf::kDiagMaxPageItems;
    const crd::perf::DiagResult res = service.execute(r);
    INFO(res.json.c_str());
    REQUIRE(res.status == crd::perf::DiagStatus::Ok);
    REQUIRE(res.complete);
    REQUIRE(json::parse({reinterpret_cast<const crd::u8*>(res.json.data()), res.json.size()}, a.doc));
    a.summary = json::find(a.doc, a.doc.root, "summary");
    a.items   = json::find(a.doc, a.doc.root, "items");
    REQUIRE(a.summary != json::kInvalid);
    REQUIRE(a.items != json::kInvalid);
}

[[nodiscard]] crd::i64 num(const Answer& a, crd::u32 node, const char* key)
{
    const crd::u32 v = json::find(a.doc, node, key);
    REQUIRE(v != json::kInvalid);
    return json::as_i64(a.doc, v, -1);
}

[[nodiscard]] bool text_is(const Answer& a, crd::u32 node, const char* key, const char* expected)
{
    return json::str_value_eq(a.doc, json::find(a.doc, node, key), expected);
}

[[nodiscard]] crd::u32 item_of(const Answer& a, const char* kind)
{
    for (crd::u32 i = 0U; i < json::count_of(a.doc, a.items); ++i)
    {
        const crd::u32 it = json::at(a.doc, a.items, i);
        if (text_is(a, it, "kind", kind))
        {
            return it;
        }
    }
    return json::kInvalid;
}

// The state the context itself reports for one mode, spelled the way the command's contract names it.
[[nodiscard]] const char* expected_mode(const g::ValidationActivation& a, g::ValidationMode m) noexcept
{
    return a.is_active(m) ? "active" : g::to_string(a.unsupported_reason(m));
}
} // namespace

TEST_CASE("diag gpu.resources: a real Vulkan context, its identities and a real frame graph",
          "[gpu-context][vulkan][frame-graph][diag][gpu]")
{
    g::GpuContextConfig cfg;
    cfg.backend           = g::GpuBackend::Vulkan;
    cfg.headless          = true;
    cfg.enable_validation = true;
    auto  ctx = g::create_vulkan_gpu_context(cfg);
    auto* vk  = ctx != nullptr ? static_cast<g::VulkanGpuContext*>(ctx.get()) : nullptr;
    if (vk == nullptr || !vk->graphics_capable() || !vk->shader_object())
    {
        SKIP("no graphics-capable Vulkan device with shader objects");
    }
    auto raster = g::create_vulkan_raster_context(*vk);
    REQUIRE(raster != nullptr);
    REQUIRE(raster->valid());
    g::ValidationCapture capture(*vk);

    crd::perf::gpu::GpuResourcesCommand command;
    crd::perf::DiagCommandService service(crd::perf::authority_bit(crd::perf::DiagAuthority::Read),
                                          crd::perf::DiagServiceConfig{}, &g_alloc);
    REQUIRE(crd::perf::gpu::register_gpu_resources(service, command));
    REQUIRE(command.add_context(*ctx));

    // The context item is what the context says about itself.
    crd::i64 baseline = 0;
    {
        Answer a;
        ask(service, a);
        const crd::u32 c = item_of(a, "context");
        REQUIRE(c != json::kInvalid);
        CHECK(text_is(a, c, "backend", "vulkan"));
        CHECK(text_is(a, c, "adapter", ctx->adapter_name()));
        const g::ValidationActivation act = ctx->validation_activation();
        CHECK(text_is(a, c, "core", expected_mode(act, g::ValidationMode::Core)));
        CHECK(text_is(a, c, "synchronization", expected_mode(act, g::ValidationMode::Synchronization)));
        CHECK(text_is(a, c, "gpu_assisted", expected_mode(act, g::ValidationMode::GpuAssisted)));
        const crd::u32 h = item_of(a, "heap");
        REQUIRE(h != json::kInvalid);
        CHECK(text_is(a, h, "status", "unavailable"));
        baseline = num(a, a.summary, "live_resources");
        CHECK(baseline == static_cast<crd::i64>(g::identity_registry().live_count(g::ObjectKind::Resource)));
    }

    // One storage buffer is one logical resource: the next snapshot shows exactly one more, and its destruction one
    // fewer.
    {
        auto sb = raster->create_storage_buffer(256U);
        REQUIRE(sb != nullptr);
        Answer a;
        ask(service, a);
        CHECK(num(a, a.summary, "live_resources") == baseline + 1);
        CHECK(num(a, item_of(a, "identities"), "live") == baseline + 1);
    }
    {
        Answer a;
        ask(service, a);
        CHECK(num(a, a.summary, "live_resources") == baseline);
    }

    // A frame graph with two equal transients written by different passes aliases them into one slot.
    auto fgraph = raster->create_frame_graph();
    REQUIRE(fgraph != nullptr);
    g::FgImageDesc desc{};
    desc.width  = 64U;
    desc.height = 64U;
    desc.format = g::FgImageFormat::RGBA8Unorm;
    const g::FgImage x = fgraph->create_transient_image(desc);
    const g::FgImage y = fgraph->create_transient_image(desc);
    REQUIRE(x.valid());
    REQUIRE(y.valid());
    fgraph->add_pass("a").writes(x).execute(&record_nothing, nullptr);
    fgraph->add_pass("b").writes(y).execute(&record_nothing, nullptr);
    REQUIRE(fgraph->build());
    fgraph->execute();
    REQUIRE(command.add_frame_graph(*fgraph, "diag-test"));
    {
        Answer a;
        ask(service, a);
        CHECK(num(a, a.summary, "frame_graphs") == 1);
        const crd::u32 fg = item_of(a, "frame-graph");
        REQUIRE(fg != json::kInvalid);
        CHECK(text_is(a, fg, "label", "diag-test"));
        const crd::i64 physical = num(a, fg, "transient_bytes");
        const crd::i64 logical  = num(a, fg, "transient_logical_bytes");
        CHECK(physical > 0);
        CHECK(physical * 2 == logical); // two disjoint equal transients share one slot
        CHECK(physical == static_cast<crd::i64>(fgraph->transient_memory_bytes()));
        CHECK(num(a, fg, "submits") == 1);
        CHECK(num(a, fg, "passes") == 2);
        CHECK(num(a, fg, "barriers") == static_cast<crd::i64>(fgraph->last_barrier_count()));
    }
    CHECK(command.remove_frame_graph(*fgraph));
    CHECK(command.remove_context(*ctx));
    CHECK(capture.error_count() == 0U);
}
