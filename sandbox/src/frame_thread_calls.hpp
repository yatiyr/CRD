#pragma once

// crd-sandbox -- DIAG.9a: calls a request thread hands to the frame-loop thread.
//
// The diagnostic panel (crd/perf/ui/diag_panel.hpp) runs each request on its own worker thread, so the frame never
// waits on a command. A device run cannot run there: on a Vulkan device without a dedicated compute family the compute
// queue is the graphics queue the frame loop submits to, and a queue is externally synchronized. FrameThreadCalls is
// the hand-over. `call` (a request thread) posts one function and blocks until the frame loop has run it; `pump` (the
// frame-loop thread, once per frame) runs the posted call, if any, to its end. One call at a time: another caller
// waits for the slot. A posted call never runs on the caller's thread.
//
// `close` (the frame-loop thread, after its last `pump` and before the panel is destroyed) refuses the posted call
// that has not started and every later one, so a worker blocked in `call` returns and the panel's join cannot wait on
// a frame loop that has ended. A run holds the frame-loop thread for its bounded length: the frame that pumps it is
// late by that much, which is the price of a device run sharing the frame loop's queue.
//
// FrameThreadDevice wraps a cook::DeviceExecutor so that its runs go through a FrameThreadCalls. It reports the
// wrapped executor's adapter. A refused call is a run that could not start (false, with the reason), so the replay
// commands answer it as the executor's refusal and record nothing.
//
// FrameThreadHost does the same for a cook::ReplayHostExecutor (the host provider's). A host run cannot run on the
// panel's worker either: the host provider's parallel ranges take their job arrays from the calling thread's
// crd::jobs frame arena, and only the pool's own threads have one (the frame-loop thread initialised the pool). The
// frame loop never resets its arena, so the wrapper puts the arena's mark back after each run: a run's job arrays are
// dead once it returns, and runs do not accumulate. A refused call is `Failed` with nothing recorded. The sandbox
// binds both wrappers over one FrameThreadCalls, so a device run and a host run never overlap either. Contract:
// docs/design/runtime-diagnostics.md (DIAG.9a).

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/core/types.hpp>

#include <condition_variable>
#include <mutex>

namespace crd::sandbox
{
class FrameThreadCalls
{
public:
    using CallFn = void (*)(void* user);

    FrameThreadCalls()                                   = default;
    ~FrameThreadCalls()                                  = default;
    FrameThreadCalls(const FrameThreadCalls&)            = delete;
    FrameThreadCalls& operator=(const FrameThreadCalls&) = delete;
    FrameThreadCalls(FrameThreadCalls&&)                 = delete;
    FrameThreadCalls& operator=(FrameThreadCalls&&)      = delete;

    // A request thread (never the frame loop's: it would wait on itself). Returns true once the frame loop ran
    // `fn(user)`, false when the calls are closed (nothing ran).
    [[nodiscard]] bool call(CallFn fn, void* user);

    // The frame-loop thread. Runs the posted call, if any, to its end. Returns whether one ran.
    bool pump();

    // The frame-loop thread. Refuses the posted call that has not started and every later call. Idempotent.
    void close();

    [[nodiscard]] bool     posted() const;    // a call waits for `pump`
    [[nodiscard]] bool     closed() const;
    [[nodiscard]] crd::u64 calls_run() const; // calls the frame loop ran
    [[nodiscard]] crd::u64 refused() const;   // calls refused by `close`

    // Closes the calls when it leaves its scope. Declared after the panel whose worker may wait in `call`, it is
    // destroyed first, so every return path closes the calls before that panel joins its worker.
    class CloseOnExit
    {
    public:
        explicit CloseOnExit(FrameThreadCalls& calls) noexcept : m_calls(&calls) {}
        ~CloseOnExit() { m_calls->close(); }
        CloseOnExit(const CloseOnExit&)            = delete;
        CloseOnExit& operator=(const CloseOnExit&) = delete;
        CloseOnExit(CloseOnExit&&)                 = delete;
        CloseOnExit& operator=(CloseOnExit&&)      = delete;

    private:
        FrameThreadCalls* m_calls;
    };

private:
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Slot : crd::u8
    {
        Empty = 0, // no call: a caller may post
        Posted,    // a call waits for `pump`
        Running,   // `pump` runs it
        Done,      // ran or refused; its caller takes the answer and empties the slot
    };

    mutable std::mutex      m_mutex;
    std::condition_variable m_changed;
    Slot                    m_slot      = Slot::Empty;
    CallFn                  m_fn        = nullptr;
    void*                   m_user      = nullptr;
    bool                    m_ran       = false; // the Done call ran (false: refused)
    bool                    m_closed    = false;
    crd::u64                m_calls_run = 0U;
    crd::u64                m_refused   = 0U;
};

class FrameThreadDevice
{
public:
    // `calls` and `inner` (with the views of its adapter) must outlive this object and every run through it.
    FrameThreadDevice(FrameThreadCalls& calls, const ceir::cook::DeviceExecutor& inner);

    // The executor to bind on the replay commands: `inner`'s adapter, its runs on the frame-loop thread.
    [[nodiscard]] const ceir::cook::DeviceExecutor& executor() const noexcept { return m_executor; }

private:
    // The executor's run: hands the inner run to the frame-loop thread and waits for it.
    static bool run(ceir::Context& ctx, const ceir::Module& module,
                    containers::ConstSpan<ceir::cook::DeviceKernelSource> kernels,
                    containers::Span<ceir::cook::DeviceBufferView> buffers, ceir::cook::DeviceRunOutcome& out,
                    containers::String& reason, void* user);

    FrameThreadCalls*          m_calls;
    ceir::cook::DeviceExecutor m_inner;
    ceir::cook::DeviceExecutor m_executor;
};

class FrameThreadHost
{
public:
    // `calls` and `inner` must outlive this object and every call through it. The frame-loop thread must be one the
    // crd::jobs pool enrolled (it initialised the pool) whenever it pumps a host call.
    FrameThreadHost(FrameThreadCalls& calls, const ceir::cook::ReplayHostExecutor& inner);

    // The executor to bind on the replay commands: `inner`'s record and replay, each run on the frame-loop thread.
    [[nodiscard]] const ceir::cook::ReplayHostExecutor& executor() const noexcept { return m_executor; }

private:
    // The executor's functions: each hands the inner call to the frame-loop thread and waits for it.
    static ceir::cook::HostExecutorStatus record(const ceir::cook::HostRecordRequest& request,
                                                 ceir::cook::ReplayRecord& out, ceir::cook::OwnedReplaySite& fault,
                                                 containers::String& missing, containers::String& reason, void* user);
    static ceir::cook::HostExecutorStatus replay(const ceir::cook::HostReplayRequest& request,
                                                 ceir::cook::HostReplayAnswer& out, containers::String& reason,
                                                 void* user);

    FrameThreadCalls*              m_calls;
    ceir::cook::ReplayHostExecutor m_inner;
    ceir::cook::ReplayHostExecutor m_executor;
};
} // namespace crd::sandbox
