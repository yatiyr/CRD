// crd-perf-ui -- DiagCommandPanel, the GUI consumer of the typed diagnostic command service.
//
// The panel is driven the way a frame loop drives it: submit, then tick until the answer arrives. Every page it shows
// is compared byte for byte with a native call of the same service at the same cursor, the grant stays the host's, a
// running request never holds a tick and the panel's cancel reaches the command's handler. No ImGui context is
// needed: `draw` is the only call that touches ImGui and it is not made here.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/ui/diag_panel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace cont = crd::containers;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;
using crd::perf::ui::DiagCommandPanel;
using crd::perf::ui::DiagPanelState;
using crd::perf::ui::DiagPanelSubmit;
using crd::perf::ui::DiagPanelTick;

namespace
{
using Clock = std::chrono::steady_clock;

constexpr crd::u32 kAnswerWaitMs  = 8000U; // a test's bound on one answer
constexpr crd::u32 kBlockBackstop = 5000U; // the blocking command gives up here, so a broken cancel fails, not hangs

[[nodiscard]] crd::u64 elapsed_ms(Clock::time_point since)
{
    return static_cast<crd::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count());
}

// --- test.items: `count` items, each {"index":i,"label":label}; takes named arguments --------------------------------

struct ItemsCommand
{
    std::atomic<crd::u64> runs{0U};
};

[[nodiscard]] bool parse_count(cont::StringView v, crd::u32& out)
{
    if (v.empty() || v.size() > 4U)
    {
        return false;
    }
    crd::u32 n = 0U;
    for (const char c : v)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        n = n * 10U + static_cast<crd::u32>(c - '0');
    }
    out = n;
    return n <= 500U;
}

DiagStatus check_items(void* /*context*/, cont::ConstSpan<crd::perf::DiagArg> args, cont::String& reason)
{
    for (const crd::perf::DiagArg& a : args)
    {
        crd::u32 n = 0U;
        if (a.name == "count")
        {
            if (!parse_count(a.value, n))
            {
                reason.append("count must be 0 to 500");
                return DiagStatus::BadArgument;
            }
        }
        else if (a.name != "label")
        {
            reason.append("test.items takes count and label");
            return DiagStatus::BadArgument;
        }
    }
    return DiagStatus::Ok;
}

DiagStatus run_items(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    auto* cmd = static_cast<ItemsCommand*>(context);
    cmd->runs.fetch_add(1U, std::memory_order_acq_rel);
    crd::u32         count = 0U;
    cont::StringView label;
    for (const crd::perf::DiagArg& a : call.request->args)
    {
        if (a.name == "count")
        {
            (void)parse_count(a.value, count);
        }
        else
        {
            label = a.value;
        }
    }
    crd::perf::DiagFields item(out.allocator());
    for (crd::u32 i = 0U; i < count; ++i)
    {
        item.clear();
        item.u64("index", i).str("label", label);
        (void)out.add_item(item);
    }
    out.summary.u64("count", count);
    return DiagStatus::Ok;
}

// --- test.block: holds the service until the caller cancels (or the backstop) ----------------------------------------

struct BlockCommand
{
    std::atomic<bool>     entered{false};
    std::atomic<bool>     left{false};
    std::atomic<crd::u64> saw_cancel{0U};
};

DiagStatus run_block(void* context, const crd::perf::DiagCall& call, crd::perf::DiagSnapshot& out)
{
    auto* cmd = static_cast<BlockCommand*>(context);
    cmd->entered.store(true, std::memory_order_release);
    const Clock::time_point start = Clock::now();
    while (!call.cancelled() && elapsed_ms(start) < kBlockBackstop)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool cancelled = call.cancelled();
    cmd->left.store(true, std::memory_order_release);
    if (cancelled)
    {
        cmd->saw_cancel.fetch_add(1U, std::memory_order_acq_rel);
        out.reason.append("cancelled by the caller");
        return DiagStatus::Cancelled;
    }
    out.reason.append("the backstop ended the block; nothing cancelled it");
    return DiagStatus::Failed;
}

const crd::perf::DiagCommandSpec kItemsSpec{"test.items", "perf-ui-test", "numbered items", DiagAuthority::Read, false};
const crd::perf::DiagCommandSpec kBlockSpec{"test.block", "perf-ui-test", "holds the service until cancelled",
                                            DiagAuthority::Read, false};

struct Host
{
    explicit Host(crd::memory::IAllocator*    alloc,
                  crd::perf::DiagAuthoritySet grant = crd::perf::authority_bit(DiagAuthority::Read))
        : service(grant, DiagServiceConfig{}, alloc)
    {
        REQUIRE(service.register_command(kItemsSpec, &run_items, &items, &check_items));
        REQUIRE(service.register_command(kBlockSpec, &run_block, &block));
    }

    ItemsCommand       items;
    BlockCommand       block;
    DiagCommandService service;
};

// Tick the panel the way a frame loop does until its answer arrives. Records the slowest tick.
[[nodiscard]] bool wait_answer(DiagCommandPanel& panel, crd::u64* slowest_tick_us = nullptr)
{
    const Clock::time_point start = Clock::now();
    while (elapsed_ms(start) < kAnswerWaitMs)
    {
        const Clock::time_point t0 = Clock::now();
        const DiagPanelTick     e  = panel.tick();
        const crd::u64          us =
            static_cast<crd::u64>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        if (slowest_tick_us != nullptr && us > *slowest_tick_us)
        {
            *slowest_tick_us = us;
        }
        if (e == DiagPanelTick::Completed)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

[[nodiscard]] bool wait_flag(const std::atomic<bool>& flag)
{
    const Clock::time_point start = Clock::now();
    while (!flag.load(std::memory_order_acquire))
    {
        if (elapsed_ms(start) > kAnswerWaitMs)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The same request, natively, at the cursor the panel's page answered.
[[nodiscard]] cont::String native_at(DiagCommandService& service, const char* command, crd::u64 cursor,
                                     crd::u32 page_items, cont::ConstSpan<crd::perf::DiagArg> args,
                                     crd::memory::IAllocator* alloc)
{
    DiagRequest r;
    r.command    = command;
    r.cursor     = cursor;
    r.page_items = page_items;
    r.args       = args;
    const DiagResult result = service.execute(r);
    return cont::String(result.json.data(), result.json.size(), alloc);
}

[[nodiscard]] cont::StringView view(const cont::String& s)
{
    return cont::StringView{s.data(), s.size()};
}
} // namespace

TEST_CASE("diag panel: lists the host's commands and shows the service's own bytes, page by page",
          "[perf-ui][diag][diag-panel]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "diag-panel-test"};
    Host                               host(&alloc);
    DiagCommandPanel                   panel(host.service, &alloc);

    // The listing is the service's, in its order, with the fake command among the built-ins.
    REQUIRE(panel.command_count() == host.service.command_count());
    bool listed = false;
    for (crd::u32 i = 0U; i < panel.command_count(); ++i)
    {
        CHECK(panel.command(i).name == host.service.command_at(i)->name);
        CHECK(panel.command(i).authority == host.service.command_at(i)->authority);
        listed = listed || panel.command(i).name == "test.items";
    }
    CHECK(listed);
    CHECK(panel.state() == DiagPanelState::Idle);

    // A label that is JSON structure once unescaped must not split an item.
    const char* const label = "a},{\"items\":[x]";
    char              args[96];
    (void)std::snprintf(args, sizeof(args), "count=70\r\nlabel=%s\n\n", label);
    REQUIRE(panel.select("test.items"));
    REQUIRE(panel.set_args(args));
    panel.set_page_items(16U);
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    REQUIRE(panel.state() == DiagPanelState::Done);
    REQUIRE(panel.result().status == DiagStatus::Ok);
    CHECK(panel.result().total == 70U);
    CHECK(panel.result().items == 16U);
    CHECK(panel.result().next_cursor != 0U);
    CHECK(host.items.runs.load() == 1U);

    const crd::perf::DiagArg native_args[2] = {{"count", "70"}, {"label", label}};
    const cont::ConstSpan<crd::perf::DiagArg> nargs(native_args, 2U);

    // Every page: the native call at the page's cursor answers the same bytes, and the panel's items are the item
    // objects the command made, in order, across pages.
    crd::u32 next_index = 0U;
    crd::u32 pages      = 0U;
    for (;;)
    {
        ++pages;
        const cont::String native = native_at(host.service, "test.items", panel.result().cursor, 16U, nargs, &alloc);
        CHECK(view(native) == view(panel.result().json));
        REQUIRE(panel.document_ok());
        CHECK(panel.document().summary == "{\"count\":70}");
        REQUIRE(panel.document().items.size() == panel.result().items);
        for (const cont::StringView item : panel.document().items)
        {
            crd::perf::DiagFields expect(&alloc);
            expect.u64("index", next_index).str("label", label);
            const cont::String e = expect.object();
            CHECK(item == view(e));
            ++next_index;
        }
        if (panel.result().next_cursor == 0U)
        {
            break;
        }
        REQUIRE(pages < 5U); // 70 items at 16 a page: a cursor that never advances fails here instead of looping
        REQUIRE(panel.next_page() == DiagPanelSubmit::Started);
        REQUIRE(wait_answer(panel));
        REQUIRE(panel.result().status == DiagStatus::Ok);
    }
    CHECK(next_index == 70U);
    CHECK(pages == 5U);
    CHECK(panel.result().complete);
    CHECK(host.items.runs.load() == 1U); // later pages are cut from the one snapshot
    CHECK(panel.next_page() == DiagPanelSubmit::NoMorePages);

    // A new snapshot replaces the old one: its first cursor, sent through the panel, is the service's stale refusal.
    const crd::u64 old_cursor = panel.result().cursor;
    REQUIRE(panel.select("diag.commands"));
    REQUIRE(panel.set_args(""));
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    REQUIRE(panel.result().status == DiagStatus::Ok);
    const cont::String listing_native =
        native_at(host.service, "diag.commands", panel.result().cursor, 16U, {}, &alloc);
    CHECK(view(listing_native) == view(panel.result().json));
    CHECK(panel.document().items.size() == panel.result().items);

    REQUIRE(panel.select("test.items"));
    REQUIRE(panel.set_args("count=70"));
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    REQUIRE(panel.page_at(old_cursor) == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    CHECK(panel.result().status == DiagStatus::StaleCursor);
    CHECK(panel.document_ok());
    CHECK(panel.document().items.empty());
    CHECK(panel.document().summary == "{}"); // a refusal's summary is empty, as the service writes it
    CHECK(panel.next_page() == DiagPanelSubmit::NoMorePages);
}

TEST_CASE("diag panel: the grant stays the host's and the form only shapes the service's request",
          "[perf-ui][diag][diag-panel]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "diag-panel-test"};
    Host                               host(&alloc);
    DiagCommandPanel                   panel(host.service, &alloc);

    // Nothing selected, an unknown command, an over-long field: refused by the panel, nothing sent.
    CHECK(panel.submit() == DiagPanelSubmit::NoCommand);
    CHECK_FALSE(panel.select("no.such.command"));
    CHECK(panel.selected() == DiagCommandPanel::kNoCommand);
    char long_path[DiagCommandPanel::kPathCapacity + 1U];
    for (char& c : long_path)
    {
        c = 'p';
    }
    long_path[DiagCommandPanel::kPathCapacity] = '\0';
    CHECK_FALSE(panel.set_path(long_path));
    CHECK(panel.path().empty());

    const crd::u64 runs_before = host.service.handler_runs();

    // capture.start needs record; the host granted read. The panel can only show the service's refusal.
    bool record_listed = false;
    for (crd::u32 i = 0U; i < panel.command_count(); ++i)
    {
        if (panel.command(i).name == "capture.start")
        {
            record_listed = true;
            CHECK_FALSE(panel.granted(i));
        }
        if (panel.command(i).name == "test.items")
        {
            CHECK(panel.granted(i));
        }
    }
    CHECK(record_listed);
    REQUIRE(panel.select("capture.start"));
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    CHECK(panel.result().status == DiagStatus::Unauthorized);
    CHECK(panel.document_ok());
    CHECK(panel.document().items.empty());

    // An argument line without '=' is the panel's form error; nothing reaches the service.
    REQUIRE(panel.select("test.items"));
    REQUIRE(panel.set_args("count=3\nlabel"));
    CHECK(panel.submit() == DiagPanelSubmit::BadForm);
    CHECK(panel.form_error().find("line 2") != cont::StringView::npos);
    CHECK(panel.state() == DiagPanelState::Done); // the last answer is still shown

    // Everything else reaches the service and is its refusal, with its reason.
    struct Refused
    {
        const char* args;
        DiagStatus  status;
    };
    const Refused refused[] = {
        {"Count=3", DiagStatus::BadArgument},                                   // a malformed name
        {"count=1\ncount=2", DiagStatus::BadArgument},                          // a repeated name
        {"count=999", DiagStatus::BadArgument},                                 // the command's own check
        {"other=1", DiagStatus::BadArgument},                                   // the command's own check
        {"a=1\nb=1\nc=1\nd=1\ne=1\nf=1\ng=1\nh=1\ni=1", DiagStatus::Oversized}, // more than kDiagMaxArgs
    };
    for (const Refused& r : refused)
    {
        REQUIRE(panel.set_args(r.args));
        REQUIRE(panel.submit() == DiagPanelSubmit::Started);
        REQUIRE(wait_answer(panel));
        CHECK(panel.result().status == r.status);
        CHECK_FALSE(panel.result().reason.empty());
    }
    // A path to a command that takes none, and arguments to one without a check.
    REQUIRE(panel.set_args(""));
    REQUIRE(panel.set_path("x.bin"));
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    CHECK(panel.result().status == DiagStatus::BadArgument);
    REQUIRE(panel.set_path(""));
    REQUIRE(panel.select("diag.commands"));
    REQUIRE(panel.set_args("page=2"));
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_answer(panel));
    CHECK(panel.result().status == DiagStatus::BadArgument);

    CHECK(host.service.handler_runs() == runs_before); // no refusal ran a handler
    CHECK(host.items.runs.load() == 0U);
}

TEST_CASE("diag panel: a running request never holds a tick, and the panel's cancel reaches the command",
          "[perf-ui][diag][diag-panel]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "diag-panel-test"};
    Host                               host(&alloc);
    DiagCommandPanel                   panel(host.service, &alloc);

    REQUIRE(panel.select("test.block"));
    const Clock::time_point start = Clock::now();
    REQUIRE(panel.submit() == DiagPanelSubmit::Started);
    REQUIRE(wait_flag(host.block.entered));
    CHECK(panel.state() == DiagPanelState::Running);

    // The command holds the service's lock now. Frames keep ticking, and nothing the frame reads takes that lock.
    crd::u64 slowest_us = 0U;
    for (crd::u32 frame = 0U; frame < 100U; ++frame)
    {
        const Clock::time_point t0 = Clock::now();
        CHECK(panel.tick() == DiagPanelTick::None);
        CHECK(panel.command_count() > 0U);
        CHECK(panel.granted(0U));
        (void)panel.refresh_commands(); // refused while running: the listing is not reread under the lock
        const crd::u64 us =
            static_cast<crd::u64>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
        slowest_us = us > slowest_us ? us : slowest_us;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(slowest_us < 200000U); // a held frame would wait for the backstop (seconds)
    CHECK_FALSE(host.block.left.load());
    CHECK(panel.submit() == DiagPanelSubmit::Busy);
    CHECK(panel.next_page() == DiagPanelSubmit::Busy);
    CHECK(panel.page_at(1U) == DiagPanelSubmit::Busy);
    CHECK_FALSE(panel.refresh_commands());

    REQUIRE(panel.cancel());
    REQUIRE(wait_answer(panel));
    CHECK(panel.result().status == DiagStatus::Cancelled);
    CHECK(host.block.saw_cancel.load() == 1U);
    CHECK(elapsed_ms(start) < kBlockBackstop); // the cancel ended it, not the backstop
    CHECK_FALSE(panel.cancel());               // nothing runs now
    CHECK(panel.refresh_commands());
}

TEST_CASE("diag panel: destroying the panel cancels and joins its running request", "[perf-ui][diag][diag-panel]")
{
    crd::memory::GrowableTlsfAllocator alloc{64ULL << 20, nullptr, "diag-panel-test"};
    Host                               host(&alloc);
    const Clock::time_point            start = Clock::now();
    {
        DiagCommandPanel panel(host.service, &alloc);
        REQUIRE(panel.select("test.block"));
        REQUIRE(panel.submit() == DiagPanelSubmit::Started);
        REQUIRE(wait_flag(host.block.entered));
    }
    CHECK(host.block.left.load()); // the destructor joined the worker
    CHECK(host.block.saw_cancel.load() == 1U);
    CHECK(elapsed_ms(start) < kBlockBackstop);
}

TEST_CASE("diag panel: the document splitter reads structure, never string contents", "[perf-ui][diag][diag-panel]")
{
    crd::memory::GrowableTlsfAllocator alloc{1ULL << 20, nullptr, "diag-panel-test"};
    crd::perf::ui::DiagDocumentView    doc(&alloc);

    REQUIRE(crd::perf::ui::split_diag_document(
        R"({"ok":true,"summary":{"n":2,"s":"}]"},"items":[{"a":"x\"},{"},{"b":[1,{"c":2}]}]})", doc));
    CHECK(doc.summary == R"({"n":2,"s":"}]"})");
    REQUIRE(doc.items.size() == 2U);
    CHECK(doc.items[0] == R"({"a":"x\"},{"})");
    CHECK(doc.items[1] == R"({"b":[1,{"c":2}]})");

    REQUIRE(crd::perf::ui::split_diag_document(R"( {"ok":false,"status":"unauthorized"} )", doc));
    CHECK(doc.summary.empty());
    CHECK(doc.items.empty());

    CHECK_FALSE(crd::perf::ui::split_diag_document(R"({"items":[{"a":1})", doc));
    CHECK(doc.items.empty());
    CHECK_FALSE(crd::perf::ui::split_diag_document(R"({"items":[{"a":"unterminated}]})", doc));
    CHECK_FALSE(crd::perf::ui::split_diag_document(R"({"a":1} trailing)", doc));
    CHECK_FALSE(crd::perf::ui::split_diag_document("", doc));
}
