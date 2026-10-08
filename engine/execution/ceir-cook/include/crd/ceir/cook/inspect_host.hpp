#pragma once

// crd-ceir-cook — DIAG.8b INSPECT HOST: the composition a headless or sandbox consumer uses to run an AUTHORED CEIR
// program under an `inspect::Session`. It owns the program's ReloadSet entry (so the session binds the generation
// number the set minted), compiles the compiled plan of one entry, binds the session's authored `file:line`
// breakpoints, and runs the plan on an EXECUTING THREAD it owns. The consumer's own thread stays the controller: it
// talks to the execution only through the session's requests (each with a timeout), so a consumer never blocks on the
// program and the program never waits on a thread that is busy answering it.
//
// ⛔ THREADS. Construct, load, start, reload and destroy on ONE thread: the controller (the constructor declares it to
// the session). The executing thread allocates its run result only from the host's own execution allocator and the
// session only from its own, so the caller's allocator is touched by the controller alone.
//
// ⛔ GENERATIONS. `load` adds the program to its ReloadSet entry or reloads it through the set's lifecycle (a HotSwap
// installs a new generation; a rejected reload keeps the last good one and its plan). Every `start` rebinds the
// session to the generation installed now, so a request naming a replaced generation is refused `StaleGeneration`.
// A load while an execution is attached is refused `Busy`: its plan and its generation's Context are in use.
//
// ⛔ RECORDING (DIAG.9a). A `start` may record its execution as a run record (`replay_record.hpp`): the controller
// re-cooks the installed generation's program into the record's immutable blob (it must cook to that generation's
// own content hash) before the executing thread starts, and the executing thread traces the run as the session's
// safe-point observer, so stops, steps and value reads leave the trace exactly as an unobserved run's. `record` then
// gives the record of the last recorded execution once it has ended: its asset, the generation that ran (still that
// generation after a later reload), the build, the arguments, each input's state, the trace and the outcome. A
// cancelled execution gives none (a replay would not stop where it stopped). A run never spans a reload: a load is
// refused while an execution is attached.
//
// ⛔ HOST INPUTS (DIAG.9a). A `start` may name the host's input seam (`crd/ceir/input.hpp`): the program's input ops
// read through it on the executing thread, once per read in program order; without one every read fails
// `InputUnavailable`. The source is the caller's and must stay valid until the execution has ended (`wait_finished`
// returned true, or the host was destroyed), so a caller declares it before the host. It runs concurrently with the
// controller, so it must not share an allocator or other state with it (THREADS): a `SeededInputs` grows its streams
// from its own allocator, never the controller's. A recorded execution keeps every
// read, delivered or not, in its record, as `replay.record` does; its random input is recorded when the record holds
// them all.
//
// ⛔ LIFETIME. The destructor cancels an execution that is still running or paused (through the session, at its next
// safe point) and joins the executing thread, so a consumer that leaves early can neither hang nor leak the thread.

#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/input.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>

#include <atomic>
#include <thread> // the executing thread (the session itself never creates one)

namespace crd::ceir::cook
{
// Why a `load` did not install a runnable program.
// NOLINTNEXTLINE(performance-enum-size)
enum class HostLoad : crd::u8
{
    Ok = 0,        // the program is installed (added, hot-swapped, or unchanged) and its entry compiled
    Busy,          // an execution is attached: nothing changed
    CookFailed,    // the source did not verify and cook (`cook_error`, `cook_site`): the last good program is kept
    LoadFailed,    // the cooked program did not load or add (`add_error`): the last good program is kept
    Rejected,      // the reload was not installed (`decision`: a contract change or an unmigrated state change)
    CompileFailed, // the installed generation has no compilable `entry` (`compile_error`): nothing can start
};
[[nodiscard]] containers::StringView host_load_name(HostLoad s) noexcept;

struct HostLoadResult
{
    HostLoad           status        = HostLoad::Ok;
    CookError          cook_error    = CookError::Ok;
    CookSite           cook_site{};
    AddError           add_error     = AddError::Ok;
    ReloadDecision     decision      = ReloadDecision::NoChange; // a reload's decision (NoChange for a first add)
    plan::CompileError compile_error = plan::CompileError::Ok;
    crd::u64           generation    = 0U; // the generation installed after the call (0 when none)
    [[nodiscard]] bool ok() const noexcept { return status == HostLoad::Ok; }
};

// DIAG.9a: whether the next execution is recorded, and how many trace events the record keeps (0: the default).
struct HostRecording
{
    bool     enabled    = false;
    crd::u32 max_events = kReplayDefaultMaxEvents;
};

// Why `record` gave no record.
// NOLINTNEXTLINE(performance-enum-size)
enum class HostRecord : crd::u8
{
    Ok = 0,
    NotRecorded,    // no execution was started with recording enabled
    Running,        // the recorded execution has not ended yet
    Cancelled,      // it was cancelled (by the controller, a script or its stop bound): a replay would not stop there
    ArtifactFailed, // the generation that ran did not re-cook to its own content hash: there is no blob to replay
};
// "ok", "not-recorded", "running", "cancelled", "artifact-failed".
[[nodiscard]] containers::StringView host_record_name(HostRecord s) noexcept;

class InspectHost
{
public:
    // `alloc` is the controller's (the ReloadSet, the compiled plan, the bind reports). `reg` installs the program's
    // dialects into every generation's Context (the ReloadSet contract).
    InspectHost(memory::IAllocator* alloc, Registrar reg, void* user,
                inspect::PauseScope scope = inspect::PauseScope::Task);
    ~InspectHost();
    InspectHost(const InspectHost&)            = delete;
    InspectHost& operator=(const InspectHost&) = delete;
    InspectHost(InspectHost&&)                 = delete;
    InspectHost& operator=(InspectHost&&)      = delete;

    // Add (first call) or reload the authored `source`, named `file`, as asset `id`, and compile its `entry`. The
    // asset id is fixed by the first successful add; a later call with another id is a LoadFailed (InvalidAssetId).
    [[nodiscard]] HostLoadResult load(AssetId id, containers::StringView source, containers::StringView file,
                                      containers::StringView entry);

    // Add an authored line breakpoint; it is resolved at the next `start` (Busy while an execution is attached).
    [[nodiscard]] inspect::Refusal add_line_breakpoint(containers::StringView file, crd::u32 line, crd::u32& out_index);

    // Rebind the session to the installed generation and start the compiled entry with `args` on the executing
    // thread. `NotBound` before a successful load, `Busy` while an execution is attached. `binds()` then reports how
    // every breakpoint resolved in this generation. `recording` records this execution (see RECORDING); a start
    // without it forgets the previous record. `inputs` is the execution's host input seam (see HOST INPUTS; null:
    // none).
    [[nodiscard]] inspect::Refusal start(containers::ConstSpan<crd::i64> args, HostRecording recording = {},
                                         const input::InputSource* inputs = nullptr);

    // The run record of the last execution started with recording, once it has ended (`out` is overwritten; its
    // arrays and strings allocate from their own allocators). Controller thread.
    [[nodiscard]] HostRecord record(ReplayRecord& out) const;

    // Wait up to `timeout_ms` for the execution to end; true once it has ended and its thread is joined (also when
    // nothing was started). The run's result is then `result()`.
    [[nodiscard]] bool wait_finished(crd::u32 timeout_ms);
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] inspect::Session&                       session() noexcept { return m_session; }
    [[nodiscard]] crd::u64                                generation() const noexcept { return m_generation; }
    [[nodiscard]] const containers::Array<inspect::BindReport>& binds() const noexcept { return m_binds; }
    [[nodiscard]] const plan::RunResult&                  result() const noexcept { return m_result; }
    // The installed generation's compiled entry (a failed run's `result().fault` indexes it).
    [[nodiscard]] const plan::CompiledPlan& compiled_plan() const noexcept { return m_compiled.plan; }
    // The compiled op whose authored origins carry `file:line` in the installed generation (`inspect::op_at_line`).
    [[nodiscard]] StableId op_at_line(containers::StringView file, crd::u32 line) const noexcept;
    // The installed generation's authored position of a stop: the first origin of the stopped instr (nullptr if none).
    [[nodiscard]] const Origin* stop_origin(const inspect::StopRecord& stop) const noexcept;
    // The file name of a position's file id in the installed generation's Context (empty when unknown).
    [[nodiscard]] containers::StringView file_path(crd::u32 file_id) const noexcept;
    [[nodiscard]] const ReloadSet& programs() const noexcept { return m_set; }

private:
    void stop_execution(); // cancel (if attached) and join the executing thread

    memory::IAllocator*                    m_alloc;
    memory::GrowableTlsfAllocator          m_session_alloc; // the session's own (it allocates under its lock)
    memory::GrowableTlsfAllocator          m_exec_alloc;    // the executing thread's run result
    inspect::Session                       m_session;
    ReloadSet                              m_set;
    AssetId                                m_asset{};
    bool                                   m_loaded     = false;
    crd::u64                               m_generation = 0U;
    plan::CompileResult                    m_compiled;
    containers::String                     m_entry; // the entry the last load compiled
    containers::String                     m_file;  // the file name the last load cooked the program under
    containers::Array<inspect::BindReport> m_binds;
    containers::Array<crd::i64>            m_args;
    plan::RunResult                        m_result;
    const input::InputSource*              m_inputs = nullptr; // the execution's host input seam (the caller's)

    // DIAG.9a: the recorded execution. The controller writes all but the trace at `start`; the executing thread
    // writes the trace (from m_exec_alloc); the controller reads it only after the execution has ended.
    bool                       m_rec_on         = false;
    bool                       m_rec_artifact   = false; // the blob cooked to the generation's content hash
    crd::u32                   m_rec_max        = kReplayDefaultMaxEvents;
    crd::u64                   m_rec_generation = 0U;
    crd::u64                   m_rec_hash       = 0U;
    containers::Array<crd::u8> m_rec_blob;
    containers::String         m_rec_entry;
    containers::String         m_rec_file;
    ReplayInput                m_rec_inputs[kReplayInputs];
    ReplayTrace                m_rec_trace;
    std::atomic<bool>                      m_done{true};
    std::thread                            m_thread; // last: it runs while the members above are alive
};
} // namespace crd::ceir::cook
