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
// ⛔ LIFETIME. The destructor cancels an execution that is still running or paused (through the session, at its next
// safe point) and joins the executing thread, so a consumer that leaves early can neither hang nor leak the thread.

#include <crd/ceir/cook/hot_reload.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
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
    // every breakpoint resolved in this generation.
    [[nodiscard]] inspect::Refusal start(containers::ConstSpan<crd::i64> args);

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
    containers::Array<inspect::BindReport> m_binds;
    containers::Array<crd::i64>            m_args;
    plan::RunResult                        m_result;
    std::atomic<bool>                      m_done{true};
    std::thread                            m_thread; // last: it runs while the members above are alive
};
} // namespace crd::ceir::cook
