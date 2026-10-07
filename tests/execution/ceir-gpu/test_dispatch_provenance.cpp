// DIAG.8a — recorded GPU work and execute refusals navigate back to the authored CEIR dispatch. With a DispatchSites
// table, execute_lowered labels every dispatch with a fresh Pass identity ("[<id>] compute.dispatch @<kernel>") and
// records which op it belongs to; a refusal records the refused op. The program is authored as text under a named file,
// run through CSE, serialized and loaded into a fresh Context before it is lowered, so every op found here must resolve
// to its authored line after the full path. Expected positions come from scanning the text, never from the parser.
// Device-free (a recording fake stands in for the device recorder); the Vulkan device leg is in
// tests/execution/ceir-gpu-vulkan/test_dispatch_provenance_vulkan.cpp. ASCII test names.

#include "dispatch_provenance_fixture.hpp"

#include <crd/ceir/gpu/execute.hpp>
#include <crd/gpu/identity_registry.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace crd;                                     // NOLINT(google-build-using-namespace)
using namespace crd::ceir;                               // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu;                          // NOLINT(google-build-using-namespace)
using namespace crd::ceir_gpu_test::dispatch_provenance; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

namespace
{
// ── device-free fakes ─────────────────────────────────────────────────────────────────────────────────────────────
struct FakePipe : crd::gpu::ComputePipeline
{
};
struct FakeBuf : crd::gpu::ComputeBuffer
{
    void* map() noexcept override { return nullptr; }
    void  unmap() noexcept override {}
};

// Records dispatches, barriers and the debug labels execute_lowered opens around each dispatch.
struct LabelRec : crd::gpu::ComputeRecorder
{
    explicit LabelRec(memory::IAllocator* alloc) : names(alloc), ids(alloc) {}

    Array<String>                   names;
    Array<crd::gpu::ObjectIdentity> ids;
    int                             open                = 0; // labels currently open
    int                             max_open            = 0;
    int                             dispatches          = 0;
    int                             labelled_dispatches = 0;
    int                             barriers            = 0;

    void copy(crd::gpu::ComputeBuffer&, crd::gpu::ComputeBuffer&, u64, u64, u64) override {}
    void barrier(crd::gpu::ComputeBuffer&, crd::gpu::ComputeAccess, crd::gpu::ComputeAccess) override
    {
        ++barriers;
    }
    void dispatch(crd::gpu::ComputePipeline&, ConstSpan<crd::gpu::ComputeBuffer*>, const void*, u32, u32, u32,
                  u32) override
    {
        ++dispatches;
        if (open > 0)
        {
            ++labelled_dispatches;
        }
    }
    bool begin_label(const crd::gpu::ObjectIdentity& id, StringView name) override
    {
        names.push_back(String(name.data(), name.size(), names.allocator()));
        ids.push_back(id);
        ++open;
        max_open = open > max_open ? open : max_open;
        return true;
    }
    void end_label() override { --open; }
};

// A recorder with no debug labels (the ComputeRecorder default).
struct PlainRec : crd::gpu::ComputeRecorder
{
    int dispatches = 0;
    void copy(crd::gpu::ComputeBuffer&, crd::gpu::ComputeBuffer&, u64, u64, u64) override {}
    void barrier(crd::gpu::ComputeBuffer&, crd::gpu::ComputeAccess, crd::gpu::ComputeAccess) override {}
    void dispatch(crd::gpu::ComputePipeline&, ConstSpan<crd::gpu::ComputeBuffer*>, const void*, u32, u32, u32,
                  u32) override
    {
        ++dispatches;
    }
};

struct Resolve
{
    const Context* ctx           = nullptr;
    FakePipe*      pipe          = nullptr;
    const char*    refuse_kernel = nullptr; // this kernel symbol resolves to nullptr
};
crd::gpu::ComputePipeline* resolve(const Operation* disp, void* user)
{
    const auto* const r  = static_cast<const Resolve*>(user);
    const AttrValue   kv = r->ctx->attr_value(disp->attr(StringView("kernel")));
    if (r->refuse_kernel != nullptr && kv.kind == AttrKind::SymbolRef && kv.s == StringView(r->refuse_kernel))
    {
        return nullptr;
    }
    return r->pipe;
}
} // namespace

TEST_CASE("diag 8a: every executed dispatch is labelled with a fresh identity that finds its authored op",
          "[ceir][ceir-gpu][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Context                       loaded(&alloc);
    Array<LoweredCommand>         commands(&alloc);
    Loaded                        l;
    author_and_load(loaded, commands, l, &alloc);
    REQUIRE(commands.size() == 3U); // dispatch, RAW barrier, dispatch
    CHECK(commands[1].kind == LoweredKind::Barrier);

    FakePipe        pipe;
    FakeBuf         bufs[4];
    ResolvedBinding binds[4];
    for (usize i = 0; i < 4U; ++i)
    {
        binds[i] = ResolvedBinding{l.buffers[i], &bufs[i]};
    }
    Resolve r{&loaded, &pipe, nullptr};

    const usize live_before = crd::gpu::identity_registry().live_count(crd::gpu::ObjectKind::Pass);
    crd::gpu::ObjectIdentity first_label{};
    {
        DispatchSites sites(&alloc);
        LabelRec      rec(&alloc);
        const ExecuteError err = execute_lowered(loaded, ConstSpan<LoweredCommand>(commands.data(), commands.size()),
                                                 rec, &resolve, &r, ConstSpan<ResolvedBinding>(binds, 4U), &sites);
        REQUIRE(err == ExecuteError::None);
        CHECK(sites.fault() == nullptr);
        CHECK(rec.dispatches == 2);
        CHECK(rec.labelled_dispatches == 2); // each dispatch was recorded inside its label
        CHECK(rec.open == 0);                // every label was closed
        CHECK(rec.max_open == 1);            // and labels do not nest across dispatches
        CHECK(rec.barriers >= 1);            // the RAW barrier still replays

        REQUIRE(sites.sites().size() == 2U);
        REQUIRE(rec.ids.size() == 2U);
        const DispatchSite& s0 = sites.sites()[0];
        const DispatchSite& s1 = sites.sites()[1];
        CHECK(s0.op == l.first);
        CHECK(s1.op == l.second);
        CHECK(s0.command == 0U);
        CHECK(s1.command == 2U);
        CHECK(s0.labelled);
        CHECK(s1.labelled);
        CHECK(s0.label.kind == crd::gpu::ObjectKind::Pass);
        CHECK(s0.label == rec.ids[0]);
        CHECK(s1.label == rec.ids[1]);
        CHECK_FALSE(s0.label == s1.label);
        CHECK(crd::gpu::identity_registry().alive(s1.label));
        CHECK(rec.names[0] == std::string_view("compute.dispatch @add_first"));
        CHECK(rec.names[1] == std::string_view("compute.dispatch @add_second"));
        CHECK(crd::gpu::identity_registry().live_count(crd::gpu::ObjectKind::Pass) == live_before + 2U);

        // A label resolves to its dispatch op, and that op to its authored line after CSE, serialize and load.
        const DispatchSite* const hit = sites.find(rec.ids[1]);
        REQUIRE(hit != nullptr);
        REQUIRE(hit->op == l.second);
        const TextPos at = authored_at(loaded, hit->op, &alloc);
        CHECK(at.line == l.second_at.line);
        CHECK(at.col == l.second_at.col);
        const String site = render_op_site(loaded, hit->op, &alloc);
        CHECK(contains(site, StringView("compute.dispatch")));
        const String want = expected_site(l.second_at, &alloc);
        CHECK(contains(site, StringView(want.data(), want.size())));

        // Identities the table did not mint, and the invalid identity, find nothing.
        const crd::gpu::ObjectIdentity other = crd::gpu::identity_registry().mint(crd::gpu::ObjectKind::Pass);
        CHECK(sites.find(other) == nullptr);
        CHECK(sites.find(crd::gpu::ObjectIdentity{}) == nullptr);
        (void)crd::gpu::identity_registry().retire(other);
        first_label = s0.label;
    }
    // The table retired every identity it minted.
    CHECK(crd::gpu::identity_registry().live_count(crd::gpu::ObjectKind::Pass) == live_before);
    CHECK_FALSE(crd::gpu::identity_registry().alive(first_label));
}

TEST_CASE("diag 8a: a recorder without debug labels still records the dispatch sites, unlabelled",
          "[ceir][ceir-gpu][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Context                       loaded(&alloc);
    Array<LoweredCommand>         commands(&alloc);
    Loaded                        l;
    author_and_load(loaded, commands, l, &alloc);

    FakePipe        pipe;
    FakeBuf         bufs[4];
    ResolvedBinding binds[4];
    for (usize i = 0; i < 4U; ++i)
    {
        binds[i] = ResolvedBinding{l.buffers[i], &bufs[i]};
    }
    Resolve       r{&loaded, &pipe, nullptr};
    DispatchSites sites(&alloc);
    PlainRec      rec;
    REQUIRE(execute_lowered(loaded, ConstSpan<LoweredCommand>(commands.data(), commands.size()), rec, &resolve, &r,
                            ConstSpan<ResolvedBinding>(binds, 4U), &sites) == ExecuteError::None);
    CHECK(rec.dispatches == 2);
    REQUIRE(sites.sites().size() == 2U);
    CHECK_FALSE(sites.sites()[0].labelled); // the gap is explicit: no label reached the device
    CHECK_FALSE(sites.sites()[1].labelled);
    CHECK(sites.sites()[1].op == l.second);
}

TEST_CASE("diag 8a: a refused execute names the refused dispatch at its authored line", "[ceir][ceir-gpu][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Context                       loaded(&alloc);
    Array<LoweredCommand>         commands(&alloc);
    Loaded                        l;
    author_and_load(loaded, commands, l, &alloc);

    FakePipe        pipe;
    FakeBuf         bufs[4];
    ResolvedBinding binds[4];
    for (usize i = 0; i < 4U; ++i)
    {
        binds[i] = ResolvedBinding{l.buffers[i], &bufs[i]};
    }
    const ConstSpan<LoweredCommand> cmds(commands.data(), commands.size());
    const String                    want = expected_site(l.second_at, &alloc);

    SECTION("an unresolved kernel")
    {
        Resolve       r{&loaded, &pipe, "add_second"};
        DispatchSites sites(&alloc);
        LabelRec      rec(&alloc);
        CHECK(execute_lowered(loaded, cmds, rec, &resolve, &r, ConstSpan<ResolvedBinding>(binds, 4U), &sites) ==
              ExecuteError::UnresolvedKernel);
        REQUIRE(sites.fault() == l.second);
        const String site = render_op_site(loaded, sites.fault(), &alloc);
        CHECK(contains(site, StringView(want.data(), want.size())));
        CHECK(sites.sites().size() == 1U); // the first dispatch was recorded before the refusal
        CHECK(rec.open == 0);

        // The same table after a successful run: the fault is cleared.
        Resolve ok{&loaded, &pipe, nullptr};
        CHECK(execute_lowered(loaded, cmds, rec, &resolve, &ok, ConstSpan<ResolvedBinding>(binds, 4U), &sites) ==
              ExecuteError::None);
        CHECK(sites.fault() == nullptr);
    }
    SECTION("an unmapped binding")
    {
        Resolve       r{&loaded, &pipe, nullptr};
        DispatchSites sites(&alloc);
        PlainRec      rec;
        // %4 (the second dispatch's output) has no buffer.
        CHECK(execute_lowered(loaded, cmds, rec, &resolve, &r, ConstSpan<ResolvedBinding>(binds, 3U), &sites) ==
              ExecuteError::UnmappedBinding);
        REQUIRE(sites.fault() == l.second);
        const TextPos at = authored_at(loaded, sites.fault(), &alloc);
        CHECK(at.line == l.second_at.line);
        CHECK(at.col == l.second_at.col);
    }
    SECTION("without a site table the refusal is unchanged")
    {
        Resolve  r{&loaded, &pipe, "add_first"};
        PlainRec rec;
        CHECK(execute_lowered(loaded, cmds, rec, &resolve, &r, ConstSpan<ResolvedBinding>(binds, 4U)) ==
              ExecuteError::UnresolvedKernel);
        CHECK(rec.dispatches == 0);
    }
}
