#pragma once

// crd-ceir — DIAG.8b runtime INSPECTION (ADR-0133 DG13): line breakpoints, stepping, safe stop and typed value
// snapshots on the two existing executors. The compiled plan is driven through `plan::RunControl` (a safe point before
// every instr); the reference interpreter through its §112 `pre` step hook (a safe point before every op). No new
// evaluator: the session only decides whether the executing thread stops at a safe point it is already given.
//
// ⛔ THREADS. A pause blocks the EXECUTING thread at the safe point, inside the executor's own call. Every
// inspection is a REQUEST: the controller (another thread) posts it, and the paused executing thread answers it from
// its own state (`plan::read_value` / `Interpreter::value_of`), so no UI or agent request dereferences executor
// memory. The controller's waits all take a timeout. A safe point on the thread declared by `connect_controller`
// never pauses (it would block the only thread that can answer), and is counted as a refused pause instead.
//
// ⛔ SCOPE. `Task` pauses the one execution that reached the safe point; everything else in the process keeps
// running. `WholeHost` additionally calls the host's freeze/thaw hooks around the pause (without them it refuses).
// `NonPausable` (real-time or GPU-bound work) never pauses: hits are counted and the run continues.
//
// ⛔ GENERATIONS. A session is BOUND to one program generation (`bind`, with the host's generation number, e.g. a
// ReloadSet handle's). Every controller request names the generation it was made for; a request for any other
// generation is refused `StaleGeneration` before any work. Breakpoints are authored `file:line` positions, resolved
// through DIAG.8a provenance on every bind, so a hot-reloaded generation REBINDS them (stable ids are pre-order and
// shift when an op is inserted, so they are not a breakpoint key).
//
// ⛔ HOST WORK. A host executor that also runs bodies on its OWN sub-interpreters (the crd-jobs HostProvider's
// parallel ranges, map_reduce fold steps and pooled launch bodies) links them through `HostLink`. Those bodies are
// DETACHED: they never pause (a pool worker held at a safe point could be the thread the submitting thread waits on,
// and the session holds one stop at a time), so a breakpoint hit there is counted and refused `DetachedBody` (a typed
// gap, never a silent miss). The host's cooperative cancel flag becomes the execution's only flag: the session's
// cancel raises it, so every detached body stops too, and a host cancel ends a paused execution. Each stop reports the
// work the host has started and not yet joined (`StopRecord::pending_jobs`), which keeps running through a `Task`
// pause.
//
// ⛔ DEVICE WORK. A host seam that RECORDS GPU work (crd-ceir-gpu's `execute_lowered`) attaches the recording with
// `begin_device`. Device work is NON-PAUSABLE whatever the session's scope: a recorded dispatch runs later on the
// device, where no safe point exists, so each one is a `device_point` that never waits. A breakpoint bound to the
// dispatch op is counted (`device_hits`) and refused `NonPausable`; a pause request while a recording is attached is
// refused `NonPausable`; a cancel is honoured (the seam stops before recording the next dispatch). A recording is
// exclusive: it is refused `Busy` while another execution (or recording) is attached, including one on its own
// thread, so a host op that records device work in the middle of an interpreter execution is not supported yet.
//
// ⛔ NATIVE DEBUGGERS. A pause is an ordinary blocking wait (mutex + condition variable): no signal, trap
// instruction or thread-context change is used, so a native debugger may attach, break and resume the process at any
// time without changing the session's state. A controller wait on an executor a native debugger holds ends in
// `Timeout`.

#include <crd/ceir/context.hpp>
#include <crd/ceir/exec.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/ceir/type.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>             // the cancel flag the executors observe, the pause request and the refusal counter
#include <condition_variable> // the pause handshake (a primitive, not a container)
#include <mutex>
#include <thread>             // std::thread::id of the controller (never creates a thread)

namespace crd::ceir::inspect
{
// NOLINTNEXTLINE(performance-enum-size)
enum class PauseScope : u8
{
    Task = 0,    // only the execution at the safe point waits
    WholeHost,   // the host's freeze/thaw hooks run around the pause (refused without them)
    NonPausable, // real-time / GPU-bound work: hits are counted, nothing waits
};
[[nodiscard]] containers::StringView pause_scope_name(PauseScope s) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class Resume : u8
{
    Continue = 0, // run to the next breakpoint or pause request
    StepInto,     // stop at the next safe point
    StepOver,     // stop at the next safe point in this call frame or an outer one
    StepOut,      // stop at the next safe point in an outer call frame
};
[[nodiscard]] containers::StringView resume_name(Resume r) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class StopReason : u8
{
    None = 0,
    Breakpoint,
    Step,
    PauseRequest,
};
[[nodiscard]] containers::StringView stop_reason_name(StopReason r) noexcept;

// Why a request (or a hit) was not carried out. Every refusal happens before any executor work.
// NOLINTNEXTLINE(performance-enum-size)
enum class Refusal : u8
{
    None = 0,
    NotBound,        // no generation has been bound yet
    StaleGeneration, // the request names a generation other than the bound one
    Busy,            // a configuration change (breakpoint, bind) while an execution is attached
    NotRunning,      // a pause request or cancel while no execution is attached
    NotPaused,       // a snapshot or resume while the execution is not paused
    NonPausable,     // the session's scope never pauses
    NoHostPause,     // WholeHost scope without the host's freeze/thaw hooks
    SameThread,      // the safe point is on the controller's own thread: pausing would block the only answering thread
    DetachedBody,    // the safe point is in a body the host runs on its own sub-interpreter: it never pauses (counted)
    Timeout,         // the wait elapsed (the executor kept running, or something else holds it)
    Finished,        // the execution ended without stopping
};
[[nodiscard]] containers::StringView refusal_name(Refusal r) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class BindStatus : u8
{
    Bound = 0,    // at least one executable site carries the line
    NoCodeAtLine, // the file is known but no executable site carries the line (a blank, a terminator, removed code)
    UnknownFile,  // the generation's Context has no such source file
};
[[nodiscard]] containers::StringView bind_status_name(BindStatus s) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class ValueStatus : u8
{
    Available = 0,  // `bits` holds the value
    NotYetComputed, // defined on the path to the safe point, later than it (the op at the safe point included)
    OutOfScope,     // not on the path to the safe point in its frame (another region, function or call frame)
    OptimizedAway,  // a pass replaced it; `survivor` is the op that now stands for it
    NoSuchValue,    // nothing defines it in this generation
    Redacted,       // available, but the host's redaction policy withholds the bits
};
[[nodiscard]] containers::StringView value_status_name(ValueStatus s) noexcept;

// NOLINTNEXTLINE(performance-enum-size)
enum class RedactionClass : u8
{
    Public = 0,
    Restricted, // the bits never leave the executing thread
};

// The host's freeze/thaw for a WholeHost pause (e.g. stop submitting frames and jobs). Called on the executing thread.
struct HostPause
{
    void (*freeze)(void* user) = nullptr;
    void (*thaw)(void* user)   = nullptr;
    void* user                 = nullptr;
};

// A host executor's link for one execution (see HOST WORK above). Called and read on the executing thread only.
struct HostLink
{
    std::atomic<bool>* cancel = nullptr;      // the host's cooperative cancel flag, shared with its detached bodies
    u32 (*pending)(void* user) = nullptr;     // host work started and not yet joined, reported on each stop
    void* user                 = nullptr;
};

// An observer of an interpreter execution under a session (e.g. a run recorder): the step hooks it would install on
// the interpreter itself, which the session's own hooks would otherwise replace. `pre` runs at every safe point before
// the session decides whether to stop there (so it also sees the op a stop holds); `post` after every successful
// dispatch. Both run on the executing thread; either may be null.
struct StepObserver
{
    exec::Interpreter::StepHook pre  = nullptr;
    exec::Interpreter::StepHook post = nullptr;
    void*                       user = nullptr;
};

// The host's redaction policy: the retained owner of a value decides whether its bits may be shown.
using RedactFn = RedactionClass (*)(StableId op, TypeId type, void* user);

inline constexpr u32 kNoBreakpoint = 0xFFFFFFFFU;

// The first compiled op of `plan` whose authored origins carry `file:line` (resolved the way a line breakpoint is: the
// file through `ctx`, the Context the plan was compiled from). Invalid when the file is unknown or no site carries the
// line. A consumer names a value to snapshot by its authored line through this, never by a stable id it guessed.
[[nodiscard]] StableId op_at_line(const plan::CompiledPlan& plan, const Context& ctx, containers::StringView file,
                                  u32 line) noexcept;

// One breakpoint's binding in the bound generation.
struct BindReport
{
    u32        breakpoint = 0U;
    BindStatus status     = BindStatus::NoCodeAtLine;
    u32        sites      = 0U; // executable sites carrying the line (a CSE survivor carries every merged line)
    StableId   first_op{};      // the first such site's op
};

// Where and why the execution stopped.
struct StopRecord
{
    u64            generation = 0U;
    u64            sequence   = 0U; // 1 for the session's first stop, then increasing
    StopReason     reason     = StopReason::None;
    StableId       op{};            // the op about to run
    u32            depth      = 0U; // call-frame depth (0 = the entry function)
    u32            breakpoint = kNoBreakpoint;
    u32            pending_jobs = 0U; // host work started and not yet joined (e.g. pooled launches not yet awaited)
    plan::InstrRef at;                // the compiled instr (plan executions only)
};

struct ValueRef
{
    StableId op{};
    u32      result = 0U;
};

// A typed value snapshot. `type_text` is the canonical type text (units included); `unit` is meaningful when
// `has_unit` (a quantity type). Bits are reported only when `status == Available`.
struct ValueSnapshot
{
    ValueStatus        status = ValueStatus::NoSuchValue;
    StableId           op{};
    u32                result = 0U;
    i64                bits   = 0;
    TypeId             type{};
    bool               has_unit = false;
    QuantityDim        unit{};
    RedactionClass     redaction = RedactionClass::Public;
    StableId           survivor{};
    containers::String type_text;
    explicit ValueSnapshot(memory::IAllocator* a) : type_text(a) {}
};

class Session
{
public:
    explicit Session(memory::IAllocator* alloc, PauseScope scope = PauseScope::Task, HostPause host = {});
    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&)                 = delete;
    Session& operator=(Session&&)      = delete;
    ~Session()                         = default;

    // ── configuration: the controller, while no execution is attached ──
    // Declare the calling thread as the one that answers diagnostic commands; a safe point on it never pauses.
    void connect_controller();
    // Add an authored `file:line` breakpoint (applied by the next bind). Busy while an execution is attached.
    [[nodiscard]] Refusal add_line_breakpoint(containers::StringView file, u32 line, u32& out_index);
    void                  set_redaction(RedactFn fn, void* user);
    // Bind generation `generation` and resolve every breakpoint in it, one report per breakpoint (`out` is cleared).
    // The plan form resolves through the plan's own sites (and `ctx`, the Context it was compiled from, for file
    // names); the module form through `resolve_provenance` for the interpreter. Busy while an execution is attached.
    [[nodiscard]] Refusal bind(const plan::CompiledPlan& plan, const Context& ctx, u64 generation,
                               containers::Array<BindReport>& out);
    [[nodiscard]] Refusal bind(const Module& m, const Context& ctx, u64 generation, containers::Array<BindReport>& out);

    // ── execution: the executing thread ──
    // Run the bound plan (a different plan runs with no breakpoints but still honours pause, step and cancel).
    [[nodiscard]] plan::RunResult run(const plan::CompiledPlan& plan, containers::ConstSpan<i64> args,
                                      memory::IAllocator* alloc);
    // The same, with an OBSERVER of every safe point (e.g. a DIAG.9a replay recorder). Its `safe_point` is called on
    // the executing thread at every safe point of the run, before the session decides whether to stop there, so it
    // sees the instr a stop holds as well as every instr that runs through; a `Cancel` it returns cancels the run.
    // Its `cancel` flag is not read: the session's own flag is the run's. Null observes nothing.
    [[nodiscard]] plan::RunResult run(const plan::CompiledPlan& plan, containers::ConstSpan<i64> args,
                                      memory::IAllocator* alloc, const plan::RunControl* observer);
    // Invoke through the reference interpreter. The session installs its step hook and cancel flag for the call and
    // removes both afterwards (an interpreter's own hooks and flag are replaced for that call).
    [[nodiscard]] exec::ExecResult invoke(exec::Interpreter& in, const Module& m, containers::StringView entry,
                                          containers::ConstSpan<i64> args);
    // The same for a host executor's submitting interpreter: `in` observes the host's cancel flag instead of the
    // session's own, the session's cancel raises it, and each stop reports `host.pending`.
    [[nodiscard]] exec::ExecResult invoke(exec::Interpreter& in, const Module& m, containers::StringView entry,
                                          containers::ConstSpan<i64> args, const HostLink& host);
    // The same with an OBSERVER of the execution (see StepObserver): its hooks run inside the session's, so a stop,
    // a step or a value read leaves what it observes unchanged. The session removes every hook afterwards.
    [[nodiscard]] exec::ExecResult invoke(exec::Interpreter& in, const Module& m, containers::StringView entry,
                                          containers::ConstSpan<i64> args, const HostLink& host,
                                          const StepObserver& observer);
    // Any thread, while an execution is attached: install the detached-body hook on a host sub-interpreter. It never
    // blocks and writes only counters, so it may run on many pool workers at once.
    void attach_detached(exec::Interpreter& sub);
    // Attach a device recording of generation `generation` (see DEVICE WORK above). Refused before any work:
    // `NotBound`, `StaleGeneration`, or `Busy` while another execution or recording is attached. Breakpoints reach
    // the recorded dispatch ops through the module form of `bind` (a compiled plan holds no dispatch).
    [[nodiscard]] Refusal begin_device(u64 generation);
    // The recording thread, before it records the dispatch `op`: never waits. A breakpoint bound to `op` is counted
    // and refused `NonPausable`. Returns true when a cancel was raised: the seam records nothing more.
    [[nodiscard]] bool device_point(const Operation& op);
    // End the recording `begin_device` attached.
    void end_device();

    // ── controller: any thread other than the executing one ──
    [[nodiscard]] Refusal request_pause(u64 generation);
    [[nodiscard]] Refusal wait_for_stop(u64 generation, u32 timeout_ms, StopRecord& out);
    [[nodiscard]] Refusal snapshot(u64 generation, ValueRef ref, ValueSnapshot& out, u32 timeout_ms);
    [[nodiscard]] Refusal resume(u64 generation, Resume how);
    [[nodiscard]] Refusal cancel(u64 generation);

    [[nodiscard]] u32     refused_pauses() const noexcept { return m_refused.load(std::memory_order_relaxed); }
    [[nodiscard]] u32     detached_hits() const noexcept { return m_detached.load(std::memory_order_relaxed); }
    [[nodiscard]] u32     device_hits() const noexcept { return m_device.load(std::memory_order_relaxed); }
    [[nodiscard]] Refusal last_refusal() const noexcept
    {
        return static_cast<Refusal>(m_last_refusal.load(std::memory_order_relaxed));
    }

private:
    // NOLINTNEXTLINE(performance-enum-size)
    enum class State : u8
    {
        Idle = 0,
        Running,
        Paused,
        Finished,
    };
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Executor : u8
    {
        None = 0,
        Plan,
        Interpreter,
        Device, // a device recording (begin_device): never pauses
    };
    struct LineBreakpoint
    {
        containers::String file;
        u32                line = 0U;
        explicit LineBreakpoint(memory::IAllocator* a) : file(a) {}
    };

    static plan::SafePointAction on_plan(const plan::CompiledPlan& plan, const plan::SafePoint& at, void* user);
    static void                  on_step(const Operation& op, void* user);
    static void                  on_post(const Operation& op, void* user);
    static void                  on_detached(const Operation& op, void* user);

    [[nodiscard]] Refusal    check(u64 generation) const noexcept; // m_mu held
    [[nodiscard]] StopReason decide(u32 breakpoint, u32 depth);    // executing thread
    [[nodiscard]] bool       hold(StopRecord rec);                 // executing thread; true = cancel
    [[nodiscard]] bool       cancel_raised() const noexcept;       // m_mu held
    void                     serve_read();                         // executing thread, m_mu held
    void                     begin_run(Executor e);
    void                     attach_locked(Executor e); // m_mu held
    void                     end_run();

    memory::IAllocator* m_alloc;
    PauseScope          m_scope;
    HostPause           m_host;
    RedactFn            m_redact      = nullptr;
    void*               m_redact_user = nullptr;
    bool                m_has_controller = false;
    std::thread::id     m_controller{};

    containers::Array<LineBreakpoint>          m_breakpoints;
    const Context*                              m_ctx  = nullptr;
    const plan::CompiledPlan*                   m_plan = nullptr;
    containers::Array<u32>                      m_bp_sites; // per plan site: breakpoint index + 1 (0 = none)
    containers::HashMap<const Operation*, u32>  m_bp_ops;   // interpreter: op → breakpoint index
    const Module*                               m_module = nullptr;
    bool                                        m_bound  = false;
    u64                                         m_generation = 0U;

    // The attached executor's kind: written by the executing thread and read by `request_pause`, both under m_mu.
    Executor m_exec = Executor::None;

    // The executing thread's own state (written and read only by it).
    Resume                    m_step       = Resume::Continue;
    u32                       m_step_depth = 0U;
    const plan::CompiledPlan* m_cur_plan   = nullptr;
    const plan::SafePoint*    m_cur_sp     = nullptr;
    const plan::RunControl*   m_observer   = nullptr; // the plan run's safe-point observer (null: none)
    StepObserver              m_step_observer;          // the interpreter execution's observer (null hooks: none)
    exec::Interpreter*        m_cur_in     = nullptr;
    const Operation*          m_cur_op     = nullptr;

    // The handshake (m_mu guards everything below except the atomics).
    mutable std::mutex      m_mu;
    std::condition_variable m_cv;
    State                   m_state = State::Idle;
    StopRecord              m_stop;
    u64                     m_stops = 0U;
    bool                    m_cmd_pending = false;
    Resume                  m_cmd         = Resume::Continue;
    bool                    m_req_pending = false;
    bool                    m_req_done    = false;
    ValueRef                m_req;
    ValueSnapshot           m_reply;
    HostLink                m_link; // the attached host execution's link (empty for a plain run or invoke)
    std::atomic<bool>       m_cancel{false};
    std::atomic<bool>       m_pause_req{false};
    std::atomic<u32>        m_refused{0U};
    std::atomic<u8>         m_last_refusal{0U};
    std::atomic<u32>        m_detached{0U};
    std::atomic<u32>        m_device{0U};
};
} // namespace crd::ceir::inspect
