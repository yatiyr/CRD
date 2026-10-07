// DIAG.8b — GPU dispatches are non-pausable at the execute_lowered seam. The two-dispatch program of
// dispatch_provenance_fixture.hpp (authored as text under a named file, CSE, serialized, loaded into a fresh Context
// and lowered) is recorded through execute_lowered with a DeviceInspect over an inspect::Session bound to the loaded
// module. The session's declared scope is Task (a CPU execution under it would pause at a bound breakpoint), yet a
// breakpoint on the second dispatch's authored line is counted and refused NonPausable and the recording never stops:
// the classification is made by the seam, not by the scope. A pause request during a recording is refused
// NonPausable, a cancel stops the recording before the next dispatch and is blamed on that dispatch's authored line,
// and a request naming another generation, an unbound session or a second recording is refused before any work.
// The recording runs on a second thread while this thread controls it; a backstop cancels a recording a broken seam
// left paused, so a failure cannot hang the suite. Device-free (recording fakes stand in for the device recorder);
// the Vulkan leg is in tests/execution/ceir-gpu-vulkan/test_dispatch_provenance_vulkan.cpp. ASCII test names.

#include "dispatch_provenance_fixture.hpp"

#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

using namespace crd;                                     // NOLINT(google-build-using-namespace)
using namespace crd::ceir;                               // NOLINT(google-build-using-namespace)
using namespace crd::ceir::gpu;                          // NOLINT(google-build-using-namespace)
using namespace crd::ceir_gpu_test::dispatch_provenance; // NOLINT(google-build-using-namespace)
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
namespace insp = crd::ceir::inspect;

namespace
{
constexpr u32 kWaitMs = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
constexpr u64 kGen    = 7U;

struct FakePipe : crd::gpu::ComputePipeline
{
};
struct FakeBuf : crd::gpu::ComputeBuffer
{
    void* map() noexcept override { return nullptr; }
    void  unmap() noexcept override {}
};

// Counts dispatches and calls `on_dispatch` (when set) after recording each one, on the recording thread.
struct HookRec : crd::gpu::ComputeRecorder
{
    int dispatches = 0;
    void (*on_dispatch)(int index, void* user) = nullptr;
    void* user                                 = nullptr;

    void copy(crd::gpu::ComputeBuffer&, crd::gpu::ComputeBuffer&, u64, u64, u64) override {}
    void barrier(crd::gpu::ComputeBuffer&, crd::gpu::ComputeAccess, crd::gpu::ComputeAccess) override {}
    void dispatch(crd::gpu::ComputePipeline&, ConstSpan<crd::gpu::ComputeBuffer*>, const void*, u32, u32, u32,
                  u32) override
    {
        const int index = dispatches++;
        if (on_dispatch != nullptr)
        {
            on_dispatch(index, user);
        }
    }
};

crd::gpu::ComputePipeline* resolve(const Operation* /*disp*/, void* user)
{
    return static_cast<FakePipe*>(user);
}

// The authored two-pass program, its lowering and the fake device side.
struct Rig
{
    explicit Rig(memory::IAllocator* alloc) : loaded(alloc), commands(alloc)
    {
        author_and_load(loaded, commands, l, alloc);
        for (usize i = 0; i < 4U; ++i)
        {
            binds[i] = ResolvedBinding{l.buffers[i], &bufs[i]};
        }
    }
    ExecuteError record(crd::gpu::ComputeRecorder& rec, DispatchSites* sites, DeviceInspect* inspect)
    {
        return execute_lowered(loaded, ConstSpan<LoweredCommand>(commands.data(), commands.size()), rec, &resolve,
                               &pipe, ConstSpan<ResolvedBinding>(binds, 4U), sites, inspect);
    }

    Context               loaded;
    Array<LoweredCommand> commands;
    Loaded                l;
    FakePipe              pipe;
    FakeBuf               bufs[4];
    ResolvedBinding       binds[4];
};

// A breakpoint on the second dispatch's authored line, bound through the module form; it must bind exactly that op.
void bind_second(insp::Session& s, Rig& rig, memory::IAllocator* alloc)
{
    u32 bp = 0U;
    REQUIRE(s.add_line_breakpoint(StringView(kFile), rig.l.second_at.line, bp) == insp::Refusal::None);
    Array<insp::BindReport> rep(alloc);
    REQUIRE(s.bind(*rig.l.module, rig.loaded, kGen, rep) == insp::Refusal::None);
    REQUIRE(rep.size() == 1U);
    REQUIRE(rep[0].status == insp::BindStatus::Bound);
    CHECK(rep[0].sites == 1U);
    CHECK(rep[0].first_op == rig.l.second->stable_id());
}

// Runs `body` on a second thread (the recording thread). If the test leaves early, or a broken seam left the
// recording paused, the backstop cancels it until the thread ends, so the join always returns.
class Worker
{
public:
    template <typename F> Worker(insp::Session& s, F&& body) : m_session(s)
    {
        m_thread = std::thread(
            [this, b = std::forward<F>(body)]() mutable
            {
                b();
                m_done.store(true);
            });
    }
    Worker(const Worker&)            = delete;
    Worker& operator=(const Worker&) = delete;
    Worker(Worker&&)                 = delete;
    Worker& operator=(Worker&&)      = delete;
    ~Worker()
    {
        join();
    }
    void join()
    {
        if (!m_thread.joinable())
        {
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        while (!m_done.load() && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        while (!m_done.load())
        {
            m_backstopped = true;
            (void)m_session.cancel(kGen);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_thread.join();
    }
    [[nodiscard]] bool backstopped() const noexcept { return m_backstopped; }

private:
    insp::Session&    m_session;
    std::atomic<bool> m_done{false};
    bool              m_backstopped = false;
    std::thread       m_thread;
};
} // namespace

TEST_CASE("diag 8b: a dispatch breakpoint under a task session is counted non-pausable and never stops",
          "[ceir][ceir-gpu][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Rig                           rig(&alloc);
    insp::Session                 s(&alloc, insp::PauseScope::Task); // a pausable scope: the seam decides
    s.connect_controller();                                          // this thread; the recording runs on another
    bind_second(s, rig, &alloc);

    HookRec       rec;
    DispatchSites sites(&alloc);
    DeviceInspect di{&s, kGen, insp::Refusal::Busy};
    ExecuteError  err = ExecuteError::UnsupportedCommand;
    {
        Worker            w(s, [&] { err = rig.record(rec, &sites, &di); });
        insp::StopRecord  stop;
        const insp::Refusal waited = s.wait_for_stop(kGen, kWaitMs, stop);
        CHECK(waited == insp::Refusal::Finished); // the recording ended without ever stopping
        w.join();
        CHECK_FALSE(w.backstopped());
    }
    REQUIRE(err == ExecuteError::None);
    CHECK(di.refusal == insp::Refusal::None);
    CHECK(rec.dispatches == 2);           // both dispatches were recorded
    CHECK(s.device_hits() == 1U);         // only the op the breakpoint bound was a hit
    CHECK(s.refused_pauses() == 1U);      // ... refused, not paused
    CHECK(s.last_refusal() == insp::Refusal::NonPausable);
    CHECK(s.detached_hits() == 0U);
    CHECK(sites.fault() == nullptr);
    REQUIRE(sites.sites().size() == 2U);
    CHECK(sites.sites()[1].op == rig.l.second);

    // The recording detached: a pause request and a cancel find nothing running, and the session can rebind.
    CHECK(s.request_pause(kGen) == insp::Refusal::NotRunning);
    CHECK(s.cancel(kGen) == insp::Refusal::NotRunning);
    Array<insp::BindReport> rep(&alloc);
    CHECK(s.bind(*rig.l.module, rig.loaded, kGen, rep) == insp::Refusal::None);
}

namespace
{
// What the recording thread does from inside the recorder after the first dispatch.
struct Midway
{
    insp::Session* session        = nullptr;
    Rig*           rig            = nullptr;
    insp::Refusal  pause          = insp::Refusal::None;
    insp::Refusal  nested_refusal = insp::Refusal::None;
    ExecuteError   nested         = ExecuteError::None;
    int            nested_dispatches = 0;
    bool           cancel         = false;
    insp::Refusal  cancelled      = insp::Refusal::Busy;
};
void midway(int index, void* user)
{
    auto& m = *static_cast<Midway*>(user);
    if (index != 0)
    {
        return;
    }
    m.pause = m.session->request_pause(kGen);
    HookRec       inner;
    DeviceInspect di{m.session, kGen, insp::Refusal::None};
    m.nested            = m.rig->record(inner, nullptr, &di);
    m.nested_refusal    = di.refusal;
    m.nested_dispatches = inner.dispatches;
    if (m.cancel)
    {
        m.cancelled = m.session->cancel(kGen);
    }
}
} // namespace

TEST_CASE("diag 8b: a recording refuses a pause, is exclusive, and a cancel stops it at the next dispatch",
          "[ceir][ceir-gpu][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Rig                           rig(&alloc);
    insp::Session                 s(&alloc, insp::PauseScope::Task);
    s.connect_controller(); // the recording runs on this thread: a seam that tried to pause is refused SameThread
    bind_second(s, rig, &alloc);

    SECTION("a pause request and a second recording are refused while the recording runs on")
    {
        Midway  m{&s, &rig};
        HookRec rec;
        rec.on_dispatch = &midway;
        rec.user        = &m;
        DeviceInspect di{&s, kGen, insp::Refusal::Busy};
        REQUIRE(rig.record(rec, nullptr, &di) == ExecuteError::None);
        CHECK(m.pause == insp::Refusal::NonPausable); // device work never pauses
        CHECK(m.nested == ExecuteError::InspectRefused);
        CHECK(m.nested_refusal == insp::Refusal::Busy); // exclusive, even on its own thread
        CHECK(m.nested_dispatches == 0);                // refused before any work
        CHECK(rec.dispatches == 2);
        CHECK(s.device_hits() == 1U);
    }
    SECTION("a cancel stops the recording before the next dispatch, blamed on its authored line")
    {
        Midway m{&s, &rig};
        m.cancel = true;
        HookRec rec;
        rec.on_dispatch = &midway;
        rec.user        = &m;
        DispatchSites sites(&alloc);
        DeviceInspect di{&s, kGen, insp::Refusal::Busy};
        CHECK(rig.record(rec, &sites, &di) == ExecuteError::Cancelled);
        CHECK(m.cancelled == insp::Refusal::None);
        CHECK(rec.dispatches == 1); // the second dispatch was never recorded
        CHECK(sites.sites().size() == 1U);
        REQUIRE(sites.fault() == rig.l.second);
        const TextPos at = authored_at(rig.loaded, sites.fault(), &alloc);
        CHECK(at.line == rig.l.second_at.line);
        CHECK(at.col == rig.l.second_at.col);
        const String site = render_op_site(rig.loaded, sites.fault(), &alloc);
        const String want = expected_site(rig.l.second_at, &alloc);
        CHECK(contains(site, StringView(want.data(), want.size())));
        CHECK(s.device_hits() == 1U); // the bound op was reached (and refused) before the cancel took effect

        // The cancelled recording detached; the next one starts uncancelled and records both dispatches.
        HookRec again;
        DeviceInspect di2{&s, kGen, insp::Refusal::Busy};
        CHECK(rig.record(again, &sites, &di2) == ExecuteError::None);
        CHECK(di2.refusal == insp::Refusal::None);
        CHECK(again.dispatches == 2);
        CHECK(sites.fault() == nullptr);
        CHECK(s.device_hits() == 2U);
    }
}

TEST_CASE("diag 8b: a recording for another generation or an unbound session is refused before any work",
          "[ceir][ceir-gpu][diag]")
{
    memory::GrowableTlsfAllocator alloc;
    Rig                           rig(&alloc);

    SECTION("an unbound session")
    {
        insp::Session s(&alloc, insp::PauseScope::Task);
        HookRec       rec;
        DispatchSites sites(&alloc);
        DeviceInspect di{&s, kGen, insp::Refusal::None};
        CHECK(rig.record(rec, &sites, &di) == ExecuteError::InspectRefused);
        CHECK(di.refusal == insp::Refusal::NotBound);
        CHECK(rec.dispatches == 0);
        CHECK(sites.sites().empty());
        CHECK(sites.fault() == nullptr);
    }
    SECTION("a stale generation")
    {
        insp::Session s(&alloc, insp::PauseScope::Task);
        s.connect_controller();
        bind_second(s, rig, &alloc);
        HookRec       rec;
        DispatchSites sites(&alloc);
        DeviceInspect di{&s, kGen + 1U, insp::Refusal::None};
        CHECK(rig.record(rec, &sites, &di) == ExecuteError::InspectRefused);
        CHECK(di.refusal == insp::Refusal::StaleGeneration);
        CHECK(rec.dispatches == 0);
        CHECK(sites.sites().empty());
        CHECK(s.device_hits() == 0U);
        // The refusal attached nothing: the bound generation records normally afterwards.
        DeviceInspect ok{&s, kGen, insp::Refusal::Busy};
        CHECK(rig.record(rec, &sites, &ok) == ExecuteError::None);
        CHECK(ok.refusal == insp::Refusal::None);
        CHECK(rec.dispatches == 2);
    }
    SECTION("without a session the recording is unchanged")
    {
        HookRec rec;
        CHECK(rig.record(rec, nullptr, nullptr) == ExecuteError::None);
        CHECK(rec.dispatches == 2);
    }
}
