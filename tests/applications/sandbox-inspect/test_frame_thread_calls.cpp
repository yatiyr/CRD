// DIAG.9a: the sandbox's device records (sandbox/src/frame_thread_calls.cpp and gpu_device_executor.cpp, compiled into
// this test the way the panel's sources are). The diagnostic panel (crd-perf-ui DiagCommandPanel) runs each request on
// its own worker thread; the sandbox binds the replay commands' device executor through FrameThreadDevice, so every
// device run is handed to the frame-loop thread, which this test thread plays: it pumps the calls and ticks the panel
// once per "frame". Covered: `replay.record executor=device` and `replay.run` of the record through the panel run the
// device program on the pumping thread, never on the panel's worker, and the record is the library's record of the
// same inputs byte for byte; replaying it against an edited kernel names the edited dispatch at its scanned line (so
// the run's outcome and buffers come back from the frame loop); a posted run waits for the frame loop; closing the
// calls refuses the posted run and every later one, nothing runs and no record is written; concurrent callers are
// served one at a time, each on the pumping thread. On a device (soft-skips without one): the sandbox's GPU executor
// (Vulkan, and DX12 on Windows) records through the panel, byte-equal to the library's record on the same executor,
// the record replays on the CPU reference within its declared envelope and reproduces on the device. Expected lines
// are scanned from the text. ASCII test names.

#include "frame_thread_calls.hpp"
#include "gpu_device_executor.hpp"

#include "../../execution/ceir-cook/device_replay_fixture.hpp"

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/gpu/context.hpp>
#include <crd/gpu/vulkan_context.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/ui/diag_panel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace
{
namespace ck = crd::ceir::cook;
namespace fx = crd::ceir_test::device_replay;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::ui::DiagCommandPanel;
using crd::perf::ui::DiagPanelSubmit;
using crd::perf::ui::DiagPanelTick;
using crd::sandbox::FrameThreadCalls;
using crd::sandbox::FrameThreadDevice;

constexpr u32 kWaitMs  = 20000U; // generous: a sanitizer lane is slow, and a pass never waits this long
constexpr u32 kCallers = 4U;     // the concurrent callers' case: threads
constexpr u32 kEach    = 50U;    // and calls per thread

// A device executor that records the thread each run ran on, around another executor.
struct Probe
{
    ck::DeviceExecutor inner;
    std::thread::id    thread;
    u32                runs = 0U;
};

bool probe_run(crd::ceir::Context& ctx, const crd::ceir::Module& module, ConstSpan<ck::DeviceKernelSource> kernels,
               Span<ck::DeviceBufferView> buffers, ck::DeviceRunOutcome& out, String& reason, void* user)
{
    auto* const p = static_cast<Probe*>(user);
    p->thread     = std::this_thread::get_id();
    ++p->runs;
    return p->inner.run(ctx, module, kernels, buffers, out, reason, p->inner.user);
}

ck::DeviceExecutor probed(Probe& p)
{
    ck::DeviceExecutor e = p.inner;
    e.run                = &probe_run;
    e.user               = &p;
    return e;
}

crd::perf::DiagServiceConfig rooted(StringView root)
{
    crd::perf::DiagServiceConfig c;
    c.root = root;
    return c;
}

// The sandbox's diagnostic host as far as the replay commands go: their configuration bound to `device`, a service
// granting what they need and the panel over it. The panel is destroyed first; the calls it may wait on must be closed
// before that (FrameThreadCalls::CloseOnExit, declared after this host).
struct PanelHost
{
    PanelHost(StringView root, const ck::DeviceExecutor* device)
        : svc(crd::perf::authority_bit(crd::perf::DiagAuthority::Execute) |
                  crd::perf::authority_bit(crd::perf::DiagAuthority::Record),
              rooted(root))
    {
        cmd.registrar = &fx::registrar;
        cmd.device    = device;
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

bool answer_has(const DiagCommandPanel& panel, StringView needle)
{
    const crd::perf::DiagResult& r = panel.result();
    return StringView{r.json.data(), r.json.size()}.find(needle) != StringView::npos;
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
    s.append("\nexecutor=device\nenvelope=ulp:32\nkernel_dir=");
    s.append(fx::kKernelDir);
    s.append("\nbuffers=");
    s.append(fx::kBufferFiles);
    s.append("\n");
    return s;
}

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

// The library's record of the fixture on `executor`, encoded.
Array<u8> library_record(const fx::Authored& a, const ck::DeviceExecutor& executor, crd::memory::IAllocator* alloc)
{
    ck::ReplayRecord rec(alloc);
    String           reason(alloc);
    const ck::DeviceReplayStatus s =
        ck::record_device_run(a.request(ck::DeviceEnvelope{ck::DeviceEnvelopeKind::Ulp, fx::kDeclaredUlps}), executor,
                              &fx::registrar, nullptr, rec, reason);
    INFO(reason.c_str());
    REQUIRE(s == ck::DeviceReplayStatus::Ok);
    Array<u8> bytes(alloc);
    ck::encode_record(rec, bytes);
    return bytes;
}

// Records the fixture through the panel on `device` (wrapping the sandbox's executor) and replays it there: the record
// is `expected` byte for byte and reproduces with no distance.
void record_and_replay_on(const ck::DeviceExecutor& device, const fx::Authored& a, const Array<u8>& expected,
                          const char* backend, const char* tag, crd::memory::IAllocator* alloc)
{
    const fx::DeviceRoot root(tag, a, alloc);
    FrameThreadCalls     calls;
    FrameThreadDevice    frame(calls, device);
    PanelHost            h(root.root(), &frame.executor());
    const FrameThreadCalls::CloseOnExit close(calls);

    const String args = record_args("device.crpl", alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{fx::kFile}, view(args)) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    INFO(h.panel->result().json.c_str());
    REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
    String backend_field(alloc);
    backend_field.append(R"("backend":")");
    backend_field.append(backend);
    backend_field.append(R"(")");
    CHECK(answer_has(*h.panel, view(backend_field)));
    const Array<u8> made       = root.read(StringView{"device.crpl"});
    const bool      same_bytes = made == expected; // a bool: no byte dump on failure
    CHECK(same_bytes);

    // Independently of the executor: the device's record replays on the CPU reference within its declared envelope.
    ck::ReplayRecord rec(alloc);
    REQUIRE(ck::decode_record({made.data(), made.size()}, rec) == ck::RecordError::Ok);
    ck::DeviceReplay on_reference(alloc);
    REQUIRE(ck::replay_device_record(rec, ck::reference_device_executor(alloc), &fx::registrar, nullptr, {},
                                     on_reference) == ck::DeviceReplayStatus::Ok);
    CHECK(on_reference.adapter_differs);
    CHECK(on_reference.divergence.kind == ck::DeviceDivergenceKind::None);
    CHECK(on_reference.compared == 3U * fx::kN);
    CHECK(on_reference.max_distance <= fx::kDeclaredUlps);

    REQUIRE(send(*h.panel, ck::kReplayRunCommand, StringView{"device.crpl"}, StringView{}) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    INFO(h.panel->result().json.c_str());
    REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
    CHECK(answer_has(*h.panel, R"("adapter_differs":false)"));
    CHECK(answer_has(*h.panel, R"("compared":768,"max_distance":0,"result":"reproduced")"));
    CHECK(calls.calls_run() == 2U);
}
} // namespace

TEST_CASE("diag 9a: the sandbox panel runs a device record on the frame-loop thread", "[sandbox][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const fx::DeviceRoot root("crd_diag9a_sandbox_device", a, &alloc);

    Probe                    probe{ck::reference_device_executor(&alloc), {}, 0U};
    const ck::DeviceExecutor inner = probed(probe);
    FrameThreadCalls         calls;
    FrameThreadDevice        frame(calls, inner);
    // The wrapped executor reports the inner adapter and runs through the calls.
    CHECK(frame.executor().run != inner.run);
    CHECK(frame.executor().adapter.backend == StringView{"cpu-reference"});
    CHECK(frame.executor().adapter.driver == ck::kReferenceDeviceVersion);

    PanelHost                           h(root.root(), &frame.executor());
    const FrameThreadCalls::CloseOnExit close(calls);
    const std::thread::id               frame_thread = std::this_thread::get_id();

    // 1. replay.record executor=device: the run is on this (the frame loop's) thread, and the record is the library's.
    const String args = record_args("device.crpl", &alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{fx::kFile}, view(args)) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    {
        INFO(h.panel->result().json.c_str());
        REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
    }
    CHECK(probe.runs == 1U);
    CHECK(probe.thread == frame_thread);
    CHECK(calls.calls_run() == 1U);
    CHECK(calls.refused() == 0U);
    CHECK(answer_has(*h.panel, R"("backend":"cpu-reference")"));
    const Array<u8> library    = library_record(a, ck::reference_device_executor(&alloc), &alloc);
    const bool      same_bytes = root.read(StringView{"device.crpl"}) == library; // a bool: no byte dump on failure
    CHECK(same_bytes);

    // 2. replay.run of that record reproduces it, again on the frame loop's thread.
    REQUIRE(send(*h.panel, ck::kReplayRunCommand, StringView{"device.crpl"}, StringView{}) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    {
        INFO(h.panel->result().json.c_str());
        REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
        CHECK(answer_has(*h.panel, R"("compared":768,"max_distance":0,"result":"reproduced")"));
    }
    CHECK(probe.runs == 2U);
    CHECK(probe.thread == frame_thread);
    CHECK(calls.calls_run() == 2U);

    // 3. Against an edited @wave (its product scaled by 1.25): the run's buffers come back from the frame loop, and the
    //    first element outside the envelope is named at @wave's dispatch.
    const String edited = fx::wave_kernel(&alloc, 1.25);
    root.text(StringView{"kernels/wave.ckir"}, view(edited));
    String against(&alloc);
    against.append("kernel_dir=");
    against.append(fx::kKernelDir);
    REQUIRE(send(*h.panel, ck::kReplayRunCommand, StringView{"device.crpl"}, view(against)) ==
            DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel));
    {
        INFO(h.panel->result().json.c_str());
        REQUIRE(h.panel->result().status == crd::perf::DiagStatus::Ok);
        CHECK(answer_has(*h.panel, R"("divergence":"element","buffer":2,"element":0,"type":"f32",)"));
        char at_line[160];
        (void)std::snprintf(at_line, sizeof(at_line), R"("dispatch_file":"%s","dispatch_line":%u,)", fx::kFile,
                            fx::locate(StringView{fx::kProgram}, StringView{fx::kWaveDispatch}).line);
        CHECK(answer_has(*h.panel, StringView{at_line}));
    }
    CHECK(probe.runs == 3U);
    CHECK(probe.thread == frame_thread);
    CHECK(calls.calls_run() == 3U);
}

TEST_CASE("diag 9a: closing the frame-loop calls refuses a waiting device run and every later one",
          "[sandbox][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const fx::DeviceRoot root("crd_diag9a_sandbox_device_closed", a, &alloc);

    Probe                               probe{ck::reference_device_executor(&alloc), {}, 0U};
    const ck::DeviceExecutor            inner = probed(probe);
    FrameThreadCalls                    calls;
    FrameThreadDevice                   frame(calls, inner);
    PanelHost                           h(root.root(), &frame.executor());
    const FrameThreadCalls::CloseOnExit close(calls);

    // The worker posts the run and waits: nothing runs until the frame loop pumps, and the panel has no answer.
    const String args = record_args("device.crpl", &alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{fx::kFile}, view(args)) == DiagPanelSubmit::Started);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (!calls.posted() && std::chrono::steady_clock::now() < until)
    {
        CHECK(h.panel->tick() == DiagPanelTick::None);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(calls.posted());
    CHECK(h.panel->tick() == DiagPanelTick::None);
    CHECK(probe.runs == 0U);

    // The frame loop ends: the posted run is refused, the worker answers without a pump, and nothing ran.
    calls.close();
    CHECK(calls.closed());
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
        CHECK(reason_has(*h.panel, "the frame loop has ended"));
    }
    CHECK(probe.runs == 0U);
    CHECK(calls.calls_run() == 0U);
    CHECK(calls.refused() == 1U);
    CHECK_FALSE(root.has(StringView{"device.crpl"}));

    // Every later run is refused at once, without the frame loop.
    const String again = record_args("again.crpl", &alloc);
    REQUIRE(send(*h.panel, ck::kReplayRecordCommand, StringView{fx::kFile}, view(again)) == DiagPanelSubmit::Started);
    REQUIRE(frames_until_answer(calls, *h.panel, false));
    CHECK(h.panel->result().status == crd::perf::DiagStatus::Failed);
    CHECK(reason_has(*h.panel, "the frame loop has ended"));
    CHECK(calls.refused() == 2U);
    CHECK(probe.runs == 0U);
    CHECK_FALSE(root.has(StringView{"again.crpl"}));
    CHECK_FALSE(calls.pump());
}

TEST_CASE("diag 9a: frame-loop calls serve concurrent callers one at a time on the pumping thread",
          "[sandbox][diag][device]")
{
    struct Shared
    {
        std::thread::id  frame;
        std::atomic<u32> inside{0U};
        std::atomic<u32> overlaps{0U};
        std::atomic<u32> elsewhere{0U};
        std::atomic<u32> ran{0U};
    };
    Shared shared;
    shared.frame = std::this_thread::get_id();
    const FrameThreadCalls::CallFn fn = [](void* user)
    {
        auto* const s = static_cast<Shared*>(user);
        if (s->inside.fetch_add(1U) != 0U)
        {
            s->overlaps.fetch_add(1U);
        }
        if (std::this_thread::get_id() != s->frame)
        {
            s->elsewhere.fetch_add(1U);
        }
        s->ran.fetch_add(1U);
        s->inside.fetch_sub(1U);
    };

    FrameThreadCalls calls;
    std::atomic<u32> answered{0U};
    std::atomic<u32> refused{0U};
    {
        std::thread callers[kCallers];
        for (std::thread& t : callers)
        {
            t = std::thread(
                [&]
                {
                    for (u32 i = 0U; i < kEach; ++i)
                    {
                        if (calls.call(fn, &shared))
                        {
                            answered.fetch_add(1U);
                        }
                        else
                        {
                            refused.fetch_add(1U);
                        }
                    }
                });
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        while (answered.load() + refused.load() < kCallers * kEach && std::chrono::steady_clock::now() < until)
        {
            if (!calls.pump())
            {
                std::this_thread::yield();
            }
        }
        calls.close(); // a caller still waiting (a failure above) returns, so the joins below cannot hang
        for (std::thread& t : callers)
        {
            t.join();
        }
    }
    CHECK(answered.load() == kCallers * kEach);
    CHECK(refused.load() == 0U);
    CHECK(shared.ran.load() == kCallers * kEach);
    CHECK(shared.overlaps.load() == 0U);
    CHECK(shared.elsewhere.load() == 0U);
    CHECK(calls.calls_run() == kCallers * kEach);

    // Closed: a call returns false at once and runs nothing.
    std::thread late([&] { CHECK_FALSE(calls.call(fn, &shared)); });
    late.join();
    CHECK(shared.ran.load() == kCallers * kEach);
    CHECK(calls.refused() == 1U);
}

TEST_CASE("diag 9a: the sandbox GPU device executor records through the panel on Vulkan",
          "[sandbox][diag][device][gpu][vulkan]")
{
    crd::gpu::GpuContextConfig cfg{};
    cfg.backend  = crd::gpu::GpuBackend::Vulkan;
    cfg.headless = true;
    auto ctx     = crd::gpu::create_vulkan_gpu_context(cfg);
    if (ctx == nullptr)
    {
        WARN("no Vulkan device available; skipping");
        return;
    }
    crd::memory::GrowableTlsfAllocator alloc;
    const auto gpu =
        crd::sandbox::GpuDeviceExecutor::vulkan(*static_cast<crd::gpu::VulkanGpuContext*>(ctx.get()), &alloc);
    REQUIRE(gpu != nullptr);
    CHECK(gpu->executor().adapter.backend == StringView{"vulkan"});
    CHECK_FALSE(gpu->executor().adapter.name.empty());
    fx::Authored a(&alloc);
    fx::author(a, &alloc);
    const Array<u8> expected = library_record(a, gpu->executor(), &alloc);
    record_and_replay_on(gpu->executor(), a, expected, "vulkan", "crd_diag9a_sandbox_gpu_vulkan", &alloc);
    CHECK(gpu->runs() == 3U);
}

#ifdef _WIN32
TEST_CASE("diag 9a: the sandbox GPU device executor records through the panel on DX12",
          "[sandbox][diag][device][gpu][dx12]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    const auto                         gpu = crd::sandbox::GpuDeviceExecutor::dx12(&alloc);
    if (gpu == nullptr)
    {
        WARN("no D3D12 device available; skipping");
        return;
    }
    CHECK(gpu->executor().adapter.backend == StringView{"dx12"});
    CHECK_FALSE(gpu->executor().adapter.name.empty());
    fx::Authored a(&alloc);
    fx::author(a, &alloc);
    const Array<u8> expected = library_record(a, gpu->executor(), &alloc);
    record_and_replay_on(gpu->executor(), a, expected, "dx12", "crd_diag9a_sandbox_gpu_dx12", &alloc);
    CHECK(gpu->runs() == 3U);
}
#endif
