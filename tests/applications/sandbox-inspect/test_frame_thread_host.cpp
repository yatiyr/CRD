// DIAG.9a: the sandbox's host records (sandbox/src/frame_thread_calls.cpp and sandbox_dialects.cpp, compiled into this
// test the way the panel's sources are). The diagnostic panel (crd-perf-ui DiagCommandPanel) runs each request on its
// own worker thread, which has no crd::jobs frame arena; the host provider's parallel ranges take their job arrays from
// the calling thread's arena. The sandbox therefore binds the host provider's executor through FrameThreadHost, so
// every host run is handed to the frame-loop thread (the thread that initialised the jobs pool), which this test thread
// plays: it initialises the pool, pumps the calls and ticks the panel once per "frame". The commands load programs with
// the sandbox's own registrar.
//
// Covered, on the committed program assets/ceir/host_replay_demo.ceir (a pooled async.launch, a task.map_reduce over a
// parallel range, a state cell and a loop whose step is the entry argument, so main(0) fails bad-for-step there):
// `replay.record executor=host` through the panel runs on the pumping thread, the run leaves that thread's frame arena
// where it found it (parallel_for reclaims its job arrays as it submits them), and the record is the library's
// `record_host_run` of the same artifact byte for byte; `replay.run` reproduces it on the pumping thread; replaying it
// against an edited copy names the edited launch constant at the await that reads it (recorded 25, observed 36), so the
// replay's answer comes back from the frame loop. Closing the calls refuses a posted host run and every later one:
// `failed`, nothing runs and no record is written. Expected lines are scanned from the text. ASCII test names.

#include "frame_thread_calls.hpp"
#include "sandbox_dialects.hpp"

#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/host/host_replay.hpp>
#include <crd/ceir/host/host_replay_diag.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/ui/diag_panel.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace
{
namespace ck = crd::ceir::cook;
namespace hs = crd::ceir::host;
namespace fs = crd::platform::fs;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::ui::DiagCommandPanel;
using crd::perf::ui::DiagPanelSubmit;
using crd::perf::ui::DiagPanelTick;
using crd::sandbox::FrameThreadCalls;
using crd::sandbox::FrameThreadHost;

constexpr u32         kWaitMs  = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
constexpr const char* kAsset   = CRD_SANDBOX_INSPECT_ENGINE_ASSETS "/ceir/host_replay_demo.ceir";
constexpr const char* kProgram = "programs/host_replay_demo.ceir";
constexpr const char* kEdited  = "programs/host_replay_demo_edited.ceir";
constexpr u32         kJobs    = 3U;

// The launched constant the edit changes, and what it becomes.
constexpr const char* kLaunchLine   = "%4 = arith.const() {value = 5} : !i32";
constexpr const char* kLaunchEdited = "%4 = arith.const() {value = 6} : !i32";

// The crd::jobs pool, initialised on this (the frame loop's) thread, which it enrolls with frame arena 0.
struct JobsPool
{
    JobsPool() { crd::jobs::init(crd::jobs::Config{.num_threads = 4}); }
    ~JobsPool() { crd::jobs::shutdown(); }
    JobsPool(const JobsPool&)            = delete;
    JobsPool& operator=(const JobsPool&) = delete;
    JobsPool(JobsPool&&)                 = delete;
    JobsPool& operator=(JobsPool&&)      = delete;
};

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

// The 1-based line holding `needle` (which names one line) in `text`.
u32 line_of(StringView text, StringView needle)
{
    const usize at = text.find(needle);
    REQUIRE(at != StringView::npos);
    REQUIRE(text.find(needle, at + 1U) == StringView::npos);
    u32 line = 1U;
    for (usize i = 0U; i < at; ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
        }
    }
    return line;
}

String edited(StringView text, crd::memory::IAllocator* alloc)
{
    const usize at = text.find(StringView{kLaunchLine});
    REQUIRE(at != StringView::npos);
    String out(alloc);
    out.append(text.substr(0U, at));
    out.append(kLaunchEdited);
    out.append(text.substr(at + StringView{kLaunchLine}.size()));
    return out;
}

// A scratch diagnostic root under the temp directory holding the program and its edited copy.
struct HostRoot
{
    fs::Path                 dir;
    crd::memory::IAllocator* alloc = nullptr;

    HostRoot(const char* tag, StringView text, crd::memory::IAllocator* allocator)
        : dir(fs::temp_directory() / StringView{tag}), alloc(allocator)
    {
        (void)fs::remove_all(dir);
        REQUIRE(fs::create_directories(dir / StringView{"programs"}));
        REQUIRE(fs::write_file_text(dir / StringView{kProgram}, text));
        const String edit = edited(text, allocator);
        REQUIRE(fs::write_file_text(dir / StringView{kEdited}, view(edit)));
    }
    ~HostRoot() { (void)fs::remove_all(dir); }
    HostRoot(const HostRoot&)            = delete;
    HostRoot& operator=(const HostRoot&) = delete;
    HostRoot(HostRoot&&)                 = delete;
    HostRoot& operator=(HostRoot&&)      = delete;

    [[nodiscard]] StringView root() const noexcept { return dir.generic(); }
    [[nodiscard]] Array<u8>  read(StringView relative) const
    {
        Array<u8> out(alloc);
        REQUIRE(fs::read_file_binary(dir / relative, out));
        return out;
    }
    [[nodiscard]] bool has(StringView relative) const { return fs::exists(dir / relative); }
};

// A host executor around another that records, for each call, the thread it ran on and the calling thread's frame
// arena mark when the inner call began and when it returned. Reached through the executor's user pointer.
struct HostProbe
{
    ck::ReplayHostExecutor inner;
    std::thread::id        thread;
    u32                    records  = 0U;
    u32                    replays  = 0U;
    usize                  mark_in  = 0U;
    usize                  mark_out = 0U;
};

ck::HostExecutorStatus probe_record(const ck::HostRecordRequest& request, ck::ReplayRecord& out,
                                    ck::OwnedReplaySite& fault, String& missing, String& reason, void* user)
{
    auto* const p = static_cast<HostProbe*>(user);
    p->thread     = std::this_thread::get_id();
    ++p->records;
    p->mark_in                     = crd::jobs::frame_get_mark();
    const ck::HostExecutorStatus s = p->inner.record(request, out, fault, missing, reason, p->inner.user);
    p->mark_out                    = crd::jobs::frame_get_mark();
    return s;
}

ck::HostExecutorStatus probe_replay(const ck::HostReplayRequest& request, ck::HostReplayAnswer& out, String& reason,
                                    void* user)
{
    auto* const p = static_cast<HostProbe*>(user);
    p->thread     = std::this_thread::get_id();
    ++p->replays;
    p->mark_in                     = crd::jobs::frame_get_mark();
    const ck::HostExecutorStatus s = p->inner.replay(request, out, reason, p->inner.user);
    p->mark_out                    = crd::jobs::frame_get_mark();
    return s;
}

crd::perf::DiagServiceConfig rooted(StringView root)
{
    crd::perf::DiagServiceConfig c;
    c.root = root;
    return c;
}

// The sandbox's diagnostic host as far as the replay commands go: their configuration over the sandbox's dialects,
// bound to `host`, a service granting what they need and the panel over it. The panel is destroyed first; the calls it
// may wait on must be closed before that (FrameThreadCalls::CloseOnExit, declared after this host).
struct PanelHost
{
    PanelHost(StringView root, const ck::ReplayHostExecutor* host)
        : svc(crd::perf::authority_bit(crd::perf::DiagAuthority::Execute) |
                  crd::perf::authority_bit(crd::perf::DiagAuthority::Record),
              rooted(root))
    {
        cmd.registrar = &crd::sandbox::register_sandbox_dialects;
        cmd.host      = host;
        REQUIRE(ck::register_replay_record(svc, cmd));
        REQUIRE(ck::register_replay_run(svc, cmd));
        panel = std::make_unique<DiagCommandPanel>(svc);
    }

    ck::ReplayCommands                cmd;
    crd::perf::DiagCommandService     svc;
    std::unique_ptr<DiagCommandPanel> panel;
};

DiagPanelSubmit send(DiagCommandPanel& panel, StringView command, StringView path, StringView args)
{
    REQUIRE(panel.select(command));
    REQUIRE(panel.set_path(path));
    REQUIRE(panel.set_args(args));
    return panel.submit();
}

// The frame loop: pump the calls (when `pump`) and tick the panel once per frame until its request answers. False when
// it has not answered after kWaitMs.
bool frames_until_answer(FrameThreadCalls& calls, DiagCommandPanel& panel, bool pump = true)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (std::chrono::steady_clock::now() < until)
    {
        if (pump)
        {
            (void)calls.pump();
        }
        if (panel.tick() == DiagPanelTick::Completed)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

StringView answer(const DiagCommandPanel& panel)
{
    const crd::perf::DiagResult& r = panel.result();
    return StringView{r.json.data(), r.json.size()};
}

bool answer_has(const DiagCommandPanel& panel, StringView needle)
{
    return answer(panel).find(needle) != StringView::npos;
}

// `needle` occurs in the answer after `anchor`.
bool answer_has_after(const DiagCommandPanel& panel, StringView anchor, StringView needle)
{
    const StringView j  = answer(panel);
    const usize      at = j.find(anchor);
    return at != StringView::npos && j.find(needle, at) != StringView::npos;
}

bool reason_has(const DiagCommandPanel& panel, StringView needle)
{
    const crd::perf::DiagResult& r = panel.result();
    return StringView{r.reason.data(), r.reason.size()}.find(needle) != StringView::npos;
}

String record_args(const char* out, crd::memory::IAllocator* alloc)
{
    String s(alloc);
    s.append("out=");
    s.append(out);
    s.append("\nargs=0\nexecutor=host\njobs=3\n");
    return s;
}

// The library's host record of the program's artifact, cooked under its relative path, encoded.
Array<u8> library_record(StringView text, crd::memory::IAllocator* alloc)
{
    crd::ceir::Context ctx(alloc);
    crd::sandbox::register_sandbox_dialects(ctx, nullptr);
    const ck::CookResult cooked = ck::cook_program_text(ctx, text, StringView{kProgram}, 1U, alloc, alloc);
    REQUIRE(cooked.ok());
    ck::ReplayRecord rec(alloc);
    hs::HostSite     fault(alloc);
    String           missing(alloc);
    const i64        zero[1] = {0};
    REQUIRE(hs::record_host_run({cooked.blob.data(), cooked.blob.size()}, StringView{kProgram}, StringView{"main"},
                                {zero, 1U}, hs::HostSchedule{kJobs, u64{1} << 20U}, ck::kReplayDefaultMaxEvents,
                                &crd::sandbox::register_sandbox_dialects, nullptr, rec, &missing,
                                &fault) == hs::HostReplayStatus::Ok);
    CHECK(missing.empty());
    Array<u8> bytes(alloc);
    ck::encode_record(rec, bytes);
    return bytes;
}

String slurp(const char* path, crd::memory::IAllocator* alloc)
{
    String text(alloc);
    REQUIRE(fs::read_file_text(fs::Path(StringView{path}), text));
    return text;
}
} // namespace

TEST_CASE("diag 9a: the sandbox panel runs a host record on the frame-loop thread", "[sandbox][diag][host]")
{
    const JobsPool                     pool;
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text       = slurp(kAsset, &alloc);
    const u32                          await_line = line_of(view(text), "%6 = async.await(%3)");
    const u32                          guard_line = line_of(view(text), "core.for(%25, %26, %2)");
    const HostRoot                     root("crd_diag9a_sandbox_host", view(text), &alloc);
    const Array<u8>                    library = library_record(view(text), &alloc);

    HostProbe        probe{hs::host_replay_executor(), {}, 0U, 0U, 0U, 0U};
    FrameThreadCalls calls;
    FrameThreadHost  frame(calls, ck::ReplayHostExecutor{&probe_record, &probe_replay, &probe});
    // The wrapper's functions hand the calls over; its user pointer is its own state, never the inner executor's.
    CHECK(frame.executor().record != &probe_record);
    CHECK(frame.executor().replay != &probe_replay);
    CHECK(frame.executor().user != &probe);

    PanelHost                           h(root.root(), &frame.executor());
    const FrameThreadCalls::CloseOnExit close(calls);
    const std::thread::id               frame_thread = std::this_thread::get_id();
    const usize                         mark         = crd::jobs::frame_get_mark();

    // 1. replay.record executor=host: the run is on this (the frame loop's) thread, it leaves this thread's frame arena
    //    where it found it, and the record is the library's.
    const String args = record_args("host.crpl", &alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{kProgram}, view(args)) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    {
        INFO(h.panel->result().json.c_str());
        REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
        CHECK(answer_has(*h.panel, R"("executor":"host")"));
        CHECK(answer_has(*h.panel, R"("error":"bad-for-step")"));
        char fault_line[64];
        (void)std::snprintf(fault_line, sizeof(fault_line), R"("fault_line":%u,)", guard_line);
        CHECK(answer_has(*h.panel, StringView{fault_line}));
    }
    CHECK(probe.records == 1U);
    CHECK(probe.thread == frame_thread);
    CHECK(probe.mark_out == probe.mark_in); // the run's parallel range reclaimed its job arrays
    CHECK(crd::jobs::frame_get_mark() == mark);
    CHECK(calls.calls_run() == 1U);
    CHECK(calls.refused() == 0U);
    const bool same_bytes = root.read(StringView{"host.crpl"}) == library; // a bool: no byte dump on failure
    CHECK(same_bytes);

    // 2. replay.run of that record reproduces it, again on the frame loop's thread.
    REQUIRE(send(*h.panel, ck::kReplayRunCommand, StringView{"host.crpl"}, StringView{}) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    {
        INFO(h.panel->result().json.c_str());
        REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
        CHECK(answer_has(*h.panel, R"("result":"reproduced")"));
        CHECK(answer_has(*h.panel, R"("executor":"host")"));
    }
    CHECK(probe.replays == 1U);
    CHECK(probe.thread == frame_thread);
    CHECK(probe.mark_out == probe.mark_in);
    CHECK(crd::jobs::frame_get_mark() == mark);
    CHECK(calls.calls_run() == 2U);

    // 3. The same inputs against the edited copy: the replay's divergence comes back from the frame loop, naming the
    //    launched constant's value at the await that reads it.
    String against(&alloc);
    against.append("program=");
    against.append(kEdited);
    REQUIRE(send(*h.panel, ck::kReplayRunCommand, StringView{"host.crpl"}, view(against)) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    {
        INFO(h.panel->result().json.c_str());
        REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
        CHECK(answer_has(*h.panel, R"("result":"diverged")"));
        const StringView item{R"("kind":"divergence")"};
        CHECK(answer_has_after(*h.panel, item, R"("divergence":"value")"));
        CHECK(answer_has_after(*h.panel, item, R"("recorded":25,)"));
        CHECK(answer_has_after(*h.panel, item, R"("observed":36,)"));
        char at_line[64];
        (void)std::snprintf(at_line, sizeof(at_line), R"("line":%u,)", await_line);
        CHECK(answer_has_after(*h.panel, item, StringView{at_line}));
    }
    CHECK(probe.replays == 2U);
    CHECK(probe.thread == frame_thread);
    CHECK(crd::jobs::frame_get_mark() == mark);
    CHECK(calls.calls_run() == 3U);
}

TEST_CASE("diag 9a: closing the frame-loop calls refuses a waiting host run and every later one",
          "[sandbox][diag][host]")
{
    const JobsPool                     pool;
    crd::memory::GrowableTlsfAllocator alloc;
    const String                       text = slurp(kAsset, &alloc);
    const HostRoot                     root("crd_diag9a_sandbox_host_closed", view(text), &alloc);

    HostProbe                           probe{hs::host_replay_executor(), {}, 0U, 0U, 0U, 0U};
    FrameThreadCalls                    calls;
    FrameThreadHost                     frame(calls, ck::ReplayHostExecutor{&probe_record, &probe_replay, &probe});
    PanelHost                           h(root.root(), &frame.executor());
    const FrameThreadCalls::CloseOnExit close(calls);

    // The worker posts the run and waits: nothing runs until the frame loop pumps, and the panel has no answer.
    const String args = record_args("host.crpl", &alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{kProgram}, view(args)) == DiagPanelSubmit::Started);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (!calls.posted() && std::chrono::steady_clock::now() < until)
    {
        CHECK(h.panel->tick() == DiagPanelTick::None);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(calls.posted());
    CHECK(h.panel->tick() == DiagPanelTick::None);
    CHECK(probe.records == 0U);

    // The frame loop ends: the posted run is refused, the worker answers without a pump, and nothing ran.
    calls.close();
    const bool answered = frames_until_answer(calls, *h.panel, false);
    CHECK(answered);
    if (!answered) // close did not release the worker: let the frame loop run it so the panel can be destroyed
    {
        (void)calls.pump();
        REQUIRE(frames_until_answer(calls, *h.panel));
    }
    {
        INFO(h.panel->result().json.c_str());
        CHECK(h.panel->result().status == crd::perf::DiagStatus::Failed);
        CHECK(reason_has(*h.panel, "the frame loop has ended: the host run did not start"));
    }
    CHECK(probe.records == 0U);
    CHECK(calls.calls_run() == 0U);
    CHECK(calls.refused() == 1U);
    CHECK_FALSE(root.has(StringView{"host.crpl"}));

    // Every later run is refused at once, without the frame loop.
    const String again = record_args("again.crpl", &alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{kProgram}, view(again)) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel, false));
    CHECK(h.panel->result().status == crd::perf::DiagStatus::Failed);
    CHECK(reason_has(*h.panel, "the frame loop has ended: the host run did not start"));
    CHECK(calls.refused() == 2U);
    CHECK(probe.records == 0U);
    CHECK_FALSE(root.has(StringView{"again.crpl"}));
    CHECK_FALSE(calls.pump());
}
