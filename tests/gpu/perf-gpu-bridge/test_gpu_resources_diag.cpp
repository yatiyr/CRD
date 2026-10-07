// The gpu.resources diagnostic command, device-free.
//
// A crd-perf DiagCommandService with the command registered answers the process-wide identity registry's live counts,
// the GPU contexts and frame graphs the host registered, and says which evidence cannot exist. The expected values
// come from the test's own fakes and from the registry the test mints into, not from the command: the identity counts
// are read before the test mints and must move by exactly what it minted; the context and frame-graph fields are the
// values the fakes were built with. Responses are parsed with the JSON reader. A real Vulkan device case lives in the
// Vulkan context suite (test_vulkan_gpu_resources_diag.cpp).

#include <crd/assetio/json.hpp>
#include <crd/gpu/context.hpp>
#include <crd/gpu/frame_graph.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/gpu/object_identity.hpp>
#include <crd/gpu/validation.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/gpu/gpu_resources_diag.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>

namespace
{
namespace g    = crd::gpu;
namespace json = crd::assetio::json;

using crd::containers::StringView;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;
using crd::perf::gpu::GpuResourcesCommand;

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc{crd::usize{16} << 20U, nullptr, "gpu-resources-diag-tests"};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

class FakeContext final : public g::IGpuContext
{
public:
    FakeContext(g::GpuBackend backend, const char* adapter, const g::ValidationActivation& activation) noexcept
        : m_backend(backend), m_adapter(adapter), m_activation(activation)
    {
    }

    [[nodiscard]] bool          valid() const noexcept override { return true; }
    [[nodiscard]] g::GpuBackend backend() const noexcept override { return m_backend; }
    [[nodiscard]] const char*   adapter_name() const noexcept override { return m_adapter; }
    [[nodiscard]] std::unique_ptr<g::IGpuProgram>
    create_program(g::ShaderStage /*stage*/, crd::containers::ConstSpan<crd::u8> /*cooked*/) override
    {
        return nullptr;
    }
    [[nodiscard]] std::unique_ptr<g::IGpuProgram>
    create_program(const crd::kir::KGraph& /*graph*/, const crd::kir::KEntry& /*entry*/) override
    {
        return nullptr;
    }
    [[nodiscard]] g::ValidationActivation validation_activation() const noexcept override { return m_activation; }

private:
    g::GpuBackend           m_backend;
    const char*             m_adapter;
    g::ValidationActivation m_activation;
};

class FakePassBuilder final : public g::IFramePassBuilder
{
public:
    g::IFramePassBuilder& reads(g::FgImage /*h*/) override { return *this; }
    g::IFramePassBuilder& reads(g::FgBuffer /*h*/) override { return *this; }
    g::IFramePassBuilder& writes(g::FgImage /*h*/) override { return *this; }
    g::IFramePassBuilder& writes(g::FgBuffer /*h*/) override { return *this; }
    g::IFramePassBuilder& read_writes(g::FgImage /*h*/) override { return *this; }
    g::IFramePassBuilder& read_writes(g::FgBuffer /*h*/) override { return *this; }
    g::IFramePassBuilder& reads_depth(g::FgImage /*h*/) override { return *this; }
    g::IFramePassBuilder& execute(g::FgExecuteFn /*fn*/, void* /*user*/) override { return *this; }
    g::IFramePassBuilder& present(g::IPresentSurface& /*surface*/) override { return *this; }
};

// A frame graph whose counters are fixed fields; everything else is inert.
class FakeFrameGraph final : public g::IFrameGraph
{
public:
    crd::u32 physical = 0U;
    crd::u32 logical  = 0U;
    crd::u32 barriers = 0U;
    crd::u32 submits  = 0U;
    crd::u32 passes   = 0U;
    crd::u32 async    = 0U;
    crd::u32 presents = 0U;
    bool     over     = false;
    bool     timing   = false;

    [[nodiscard]] g::FgImage  import_target(g::IRasterTarget& /*t*/) override { return g::FgImage{0U}; }
    [[nodiscard]] g::FgBuffer import_storage(g::IStorageBuffer& /*b*/) override { return g::FgBuffer{0U}; }
    [[nodiscard]] g::FgImage  create_transient_image(const g::FgImageDesc& /*d*/) override { return g::FgImage{0U}; }
    [[nodiscard]] g::FgBuffer create_transient_buffer(crd::u32 /*bytes*/) override { return g::FgBuffer{0U}; }
    [[nodiscard]] g::IFramePassBuilder& add_pass(const char* /*name*/, g::FgPassKind /*kind*/) override
    {
        return m_builder;
    }
    [[nodiscard]] bool build() override { return true; }
    void               execute() override {}
    void               reset() override {}

    [[nodiscard]] crd::u32 last_barrier_count() const noexcept override { return barriers; }
    [[nodiscard]] crd::u32 last_submit_count() const noexcept override { return submits; }
    [[nodiscard]] crd::u32 transient_memory_bytes() const noexcept override { return physical; }
    [[nodiscard]] crd::u32 transient_logical_bytes() const noexcept override { return logical; }
    [[nodiscard]] crd::u32 pass_count() const noexcept override { return passes; }
    [[nodiscard]] bool     gpu_timing_available() const noexcept override { return timing; }
    [[nodiscard]] crd::u32 last_present_count() const noexcept override { return presents; }
    [[nodiscard]] crd::u32 last_async_pass_count() const noexcept override { return async; }
    [[nodiscard]] bool     last_build_exceeded_budget() const noexcept override { return over; }

private:
    FakePassBuilder m_builder;
};

// A parsed response: the summary object and the items array.
struct Answer
{
    json::JsonDoc doc{&g_alloc};
    crd::u32      summary = json::kInvalid;
    crd::u32      items   = json::kInvalid;
};

void parse_answer(const DiagResult& r, Answer& a)
{
    REQUIRE(json::parse({reinterpret_cast<const crd::u8*>(r.json.data()), r.json.size()}, a.doc));
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

[[nodiscard]] bool flag(const Answer& a, crd::u32 node, const char* key)
{
    const crd::u32 v = json::find(a.doc, node, key);
    REQUIRE(v != json::kInvalid);
    REQUIRE(a.doc.nodes[v].type == json::JsonType::Bool);
    return json::as_bool(a.doc, v, false);
}

[[nodiscard]] bool text_is(const Answer& a, crd::u32 node, const char* key, const char* expected)
{
    return json::str_value_eq(a.doc, json::find(a.doc, node, key), expected);
}

[[nodiscard]] bool has_text(const Answer& a, crd::u32 node, const char* key)
{
    const crd::u32 v = json::find(a.doc, node, key);
    return v != json::kInvalid && a.doc.nodes[v].type == json::JsonType::String && a.doc.nodes[v].str_len > 0U;
}

// The `nth` item of `kind`, or kInvalid.
[[nodiscard]] crd::u32 item_of(const Answer& a, const char* kind, crd::u32 nth = 0U)
{
    for (crd::u32 i = 0U; i < json::count_of(a.doc, a.items); ++i)
    {
        const crd::u32 it = json::at(a.doc, a.items, i);
        if (text_is(a, it, "kind", kind))
        {
            if (nth == 0U)
            {
                return it;
            }
            --nth;
        }
    }
    return json::kInvalid;
}

[[nodiscard]] crd::u32 count_kind(const Answer& a, const char* kind)
{
    crd::u32 n = 0U;
    for (crd::u32 i = 0U; i < json::count_of(a.doc, a.items); ++i)
    {
        if (text_is(a, json::at(a.doc, a.items, i), "kind", kind))
        {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] DiagRequest whole()
{
    DiagRequest r;
    r.command    = crd::perf::gpu::kGpuResourcesCommand;
    r.page_items = crd::perf::kDiagMaxPageItems;
    return r;
}

[[nodiscard]] g::ValidationActivation vulkan_like() noexcept
{
    g::ValidationActivation a;
    const auto core = static_cast<crd::usize>(g::ValidationMode::Core);
    const auto sync = static_cast<crd::usize>(g::ValidationMode::Synchronization);
    a.requested[core] = true;
    a.active[core]    = true;
    a.reason[core]    = g::ValidationUnsupportedReason::None;
    a.requested[sync] = true;
    a.reason[sync]    = g::ValidationUnsupportedReason::ExtensionAbsent;
    return a;
}
} // namespace

TEST_CASE("diag gpu.resources: live identities, the host's contexts and its frame graphs", "[perf][gpu][diag]")
{
    GpuResourcesCommand command;
    DiagCommandService  service(crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &g_alloc);
    REQUIRE(crd::perf::gpu::register_gpu_resources(service, command));

    FakeContext vk(g::GpuBackend::Vulkan, "Fake Vulkan Adapter", vulkan_like());
    FakeContext dx(g::GpuBackend::Dx12, "Fake DX12 Adapter", g::ValidationActivation{});
    REQUIRE(command.add_context(vk));
    REQUIRE(command.add_context(dx));
    CHECK_FALSE(command.add_context(vk)); // already registered

    FakeFrameGraph graph;
    graph.physical = 4096U;
    graph.logical  = 12288U;
    graph.barriers = 3U;
    graph.submits  = 1U;
    graph.passes   = 2U;
    graph.async    = 1U;
    graph.presents = 0U;
    graph.over     = false;
    graph.timing   = true;
    REQUIRE(command.add_frame_graph(graph, "main"));
    CHECK_FALSE(command.add_frame_graph(graph, "again"));

    // The registry is process-wide: read the baseline, then mint and expect exactly that many more.
    g::IdentityRegistry& reg       = g::identity_registry();
    const crd::i64       base_res  = static_cast<crd::i64>(reg.live_count(g::ObjectKind::Resource));
    const crd::i64       base_prog = static_cast<crd::i64>(reg.live_count(g::ObjectKind::Program));
    const crd::i64       base_pass = static_cast<crd::i64>(reg.live_count(g::ObjectKind::Pass));
    const g::ObjectIdentity r0 = reg.mint(g::ObjectKind::Resource);
    const g::ObjectIdentity r1 = reg.mint(g::ObjectKind::Resource);
    const g::ObjectIdentity p0 = reg.mint(g::ObjectKind::Program);
    const g::ObjectIdentity s0 = reg.mint(g::ObjectKind::Pass);

    {
        const DiagResult r = service.execute(whole());
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(r.total == 3U + 2U * 2U + 1U); // identities, a context and a heap item per context, one graph
        CHECK(r.complete);
        Answer a;
        parse_answer(r, a);

        CHECK(num(a, a.summary, "live_resources") == base_res + 2);
        CHECK(num(a, a.summary, "live_programs") == base_prog + 1);
        CHECK(num(a, a.summary, "live_passes") == base_pass + 1);
        CHECK(num(a, a.summary, "contexts") == 2);
        CHECK(num(a, a.summary, "frame_graphs") == 1);
        CHECK(text_is(a, a.summary, "heap_usage", "unavailable"));

        const char*    objects[] = {"res", "prog", "pass"};
        const crd::i64 lives[]   = {base_res + 2, base_prog + 1, base_pass + 1};
        for (crd::u32 k = 0U; k < 3U; ++k)
        {
            const crd::u32 it = item_of(a, "identities", k);
            REQUIRE(it != json::kInvalid);
            CHECK(text_is(a, it, "object", objects[k]));
            CHECK(text_is(a, it, "scope", "process"));
            CHECK(num(a, it, "live") == lives[k]);
        }

        const crd::u32 c0 = item_of(a, "context", 0U);
        const crd::u32 c1 = item_of(a, "context", 1U);
        REQUIRE(c0 != json::kInvalid);
        REQUIRE(c1 != json::kInvalid);
        CHECK(num(a, c0, "index") == 0);
        CHECK(text_is(a, c0, "backend", "vulkan"));
        CHECK(text_is(a, c0, "adapter", "Fake Vulkan Adapter"));
        CHECK(flag(a, c0, "valid"));
        CHECK(text_is(a, c0, "core", "active"));
        CHECK(text_is(a, c0, "synchronization", "extension-absent"));
        CHECK(text_is(a, c0, "gpu_assisted", "not-requested"));
        CHECK(num(a, c1, "index") == 1);
        CHECK(text_is(a, c1, "backend", "dx12"));
        CHECK(text_is(a, c1, "adapter", "Fake DX12 Adapter"));
        CHECK(text_is(a, c1, "core", "not-requested"));

        CHECK(count_kind(a, "heap") == 2U);
        for (crd::u32 i = 0U; i < 2U; ++i)
        {
            const crd::u32 h = item_of(a, "heap", i);
            CHECK(num(a, h, "context") == static_cast<crd::i64>(i));
            CHECK(text_is(a, h, "status", "unavailable"));
            CHECK(has_text(a, h, "reason"));
        }

        const crd::u32 fg = item_of(a, "frame-graph");
        REQUIRE(fg != json::kInvalid);
        CHECK(text_is(a, fg, "label", "main"));
        CHECK(json::find(a.doc, fg, "status") == json::kInvalid); // a registered graph is evidence, not unavailable
        CHECK(num(a, fg, "transient_bytes") == 4096);
        CHECK(num(a, fg, "transient_logical_bytes") == 12288);
        CHECK_FALSE(flag(a, fg, "budget_exceeded"));
        CHECK(num(a, fg, "barriers") == 3);
        CHECK(num(a, fg, "submits") == 1);
        CHECK(num(a, fg, "passes") == 2);
        CHECK(num(a, fg, "async_passes") == 1);
        CHECK(num(a, fg, "presents") == 0);
        CHECK(flag(a, fg, "gpu_timing"));
    }

    // The answer follows the registry and the host's lists: retire, remove, and the next snapshot says so.
    CHECK(reg.retire(r0));
    CHECK(reg.retire(r1));
    CHECK(reg.retire(p0));
    CHECK(reg.retire(s0));
    CHECK(command.remove_context(vk));
    CHECK_FALSE(command.remove_context(vk));
    CHECK(command.remove_frame_graph(graph));
    graph.over = true; // a removed graph is never read again
    {
        const DiagResult r = service.execute(whole());
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(r.total == 3U + 2U + 1U);
        Answer a;
        parse_answer(r, a);
        CHECK(num(a, a.summary, "live_resources") == base_res);
        CHECK(num(a, a.summary, "live_programs") == base_prog);
        CHECK(num(a, a.summary, "live_passes") == base_pass);
        CHECK(num(a, a.summary, "contexts") == 1);
        const crd::u32 c0 = item_of(a, "context");
        CHECK(num(a, c0, "index") == 0);
        CHECK(text_is(a, c0, "backend", "dx12"));
        const crd::u32 fg = item_of(a, "frame-graph");
        REQUIRE(fg != json::kInvalid);
        CHECK(text_is(a, fg, "status", "unavailable"));
        CHECK(has_text(a, fg, "reason"));
        CHECK(json::find(a.doc, fg, "budget_exceeded") == json::kInvalid);
    }

    // No context at all: one unavailable context item, and no heap item to claim anything about.
    CHECK(command.remove_context(dx));
    {
        const DiagResult r = service.execute(whole());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(r.total == 3U + 1U + 1U);
        Answer a;
        parse_answer(r, a);
        CHECK(num(a, a.summary, "contexts") == 0);
        const crd::u32 c = item_of(a, "context");
        REQUIRE(c != json::kInvalid);
        CHECK(text_is(a, c, "status", "unavailable"));
        CHECK(has_text(a, c, "reason"));
        CHECK(count_kind(a, "heap") == 0U);
    }
    CHECK(command.runs() == 3U);
}

TEST_CASE("diag gpu.resources: refusals come before the command, and pages are deterministic", "[perf][gpu][diag]")
{
    GpuResourcesCommand command;
    FakeContext         vk(g::GpuBackend::Vulkan, "Fake Vulkan Adapter", vulkan_like());
    FakeFrameGraph      graph;
    REQUIRE(command.add_context(vk));
    REQUIRE(command.add_frame_graph(graph, "main"));

    // Read is the command's authority: a host that granted only record is refused before the command runs.
    DiagCommandService record_only(crd::perf::authority_bit(DiagAuthority::Record), DiagServiceConfig{}, &g_alloc);
    REQUIRE(crd::perf::gpu::register_gpu_resources(record_only, command));
    CHECK(record_only.execute(whole()).status == DiagStatus::Unauthorized);

    DiagCommandService service(crd::perf::authority_bit(DiagAuthority::Read), DiagServiceConfig{}, &g_alloc);
    REQUIRE(crd::perf::gpu::register_gpu_resources(service, command));
    CHECK_FALSE(crd::perf::gpu::register_gpu_resources(service, command)); // a duplicate name

    DiagRequest oversized = whole();
    oversized.page_items  = crd::perf::kDiagMaxPageItems + 1U;
    CHECK(service.execute(oversized).status == DiagStatus::Oversized);
    DiagRequest with_path = whole();
    with_path.path        = "x.bin";
    CHECK(service.execute(with_path).status == DiagStatus::BadArgument);
    DiagRequest schema    = whole();
    schema.schema_version = crd::perf::kDiagCommandSchemaVersion + 1U;
    CHECK(service.execute(schema).status == DiagStatus::UnsupportedSchema);
    const std::atomic<bool> cancel{true};
    CHECK(service.execute(whole(), &cancel).status == DiagStatus::Cancelled);
    CHECK(command.runs() == 0U);
    CHECK(service.handler_runs() == 0U);

    // Pages of two are cut from one snapshot: the same cursor gives the same bytes, and only the first page runs.
    DiagRequest first = whole();
    first.page_items  = 2U;
    const DiagResult p0 = service.execute(first);
    REQUIRE(p0.status == DiagStatus::Ok);
    CHECK(p0.total == 3U + 2U + 1U);
    REQUIRE(p0.next_cursor != 0U);
    DiagRequest next = first;
    next.cursor      = p0.next_cursor;
    const DiagResult p1a = service.execute(next);
    const DiagResult p1b = service.execute(next);
    REQUIRE(p1a.status == DiagStatus::Ok);
    CHECK(StringView{p1a.json.data(), p1a.json.size()} == StringView{p1b.json.data(), p1b.json.size()});
    CHECK(command.runs() == 1U);

    // Another command's snapshot makes the old cursor stale, still without running this command.
    DiagRequest listing;
    listing.command = "diag.commands";
    REQUIRE(service.execute(listing).status == DiagStatus::Ok);
    CHECK(service.execute(next).status == DiagStatus::StaleCursor);
    CHECK(command.runs() == 1U);

    // The host's lists are bounded.
    GpuResourcesCommand bounded;
    FakeContext         many[crd::perf::gpu::kGpuResourcesMaxContexts + 1U] = {
        {g::GpuBackend::Vulkan, "a", {}}, {g::GpuBackend::Vulkan, "b", {}}, {g::GpuBackend::Vulkan, "c", {}},
        {g::GpuBackend::Vulkan, "d", {}}, {g::GpuBackend::Vulkan, "e", {}}, {g::GpuBackend::Vulkan, "f", {}},
        {g::GpuBackend::Vulkan, "g", {}}, {g::GpuBackend::Vulkan, "h", {}}, {g::GpuBackend::Vulkan, "i", {}}};
    for (crd::u32 i = 0U; i < crd::perf::gpu::kGpuResourcesMaxContexts; ++i)
    {
        REQUIRE(bounded.add_context(many[i]));
    }
    CHECK_FALSE(bounded.add_context(many[crd::perf::gpu::kGpuResourcesMaxContexts]));
    CHECK(bounded.context_count() == crd::perf::gpu::kGpuResourcesMaxContexts);
}
