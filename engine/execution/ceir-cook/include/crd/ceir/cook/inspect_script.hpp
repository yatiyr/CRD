#pragma once

// crd-ceir-cook -- a scripted inspection of an authored program on an InspectHost.
//
// The one engine behind every consumer that runs an authored program to a script rather than a person: the ceridc
// `inspect` verb and the `program.inspect` diagnostic command. The caller loads the program into its InspectHost; the
// script adds the authored `file:line` breakpoints, starts the entry, and at every stop records the stop's authored
// position and depth and each watched line's typed value (read by the paused executing thread), then applies the next
// scripted action. The calling thread is the controller the host declared, so every wait is the session's bounded
// request and nothing here blocks on the program.
//
// Bounds: at most `max_stops` stops are recorded; the next stop cancels the run and marks it truncated. Every wait (a
// stop, a value, the end) is bounded by `wait_ms`. Every stop is waited for in short slices that check the caller's
// cancel flag first, so a flag raised while the program runs, or at a stop, cancels the session at its next safe point
// and the report says the caller cancelled. Contract: docs/design/runtime-diagnostics.md.

#include <crd/ceir/cook/inspect_host.hpp>
#include <crd/ceir/inspect.hpp>
#include <crd/ceir/plan.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>

namespace crd::ceir::cook
{
// What the controller does at a stop.
// NOLINTNEXTLINE(performance-enum-size)
enum class ScriptAction : u8
{
    Continue = 0,
    Into,
    Over,
    Out,
    Cancel,
};
// "continue", "into", "over", "out", "cancel".
[[nodiscard]] containers::StringView script_action_name(ScriptAction a) noexcept;
[[nodiscard]] bool                   parse_script_action(containers::StringView text, ScriptAction& out) noexcept;

inline constexpr u32 kScriptDefaultMaxStops = 64U;
inline constexpr u32 kScriptDefaultWaitMs   = 20000U;

// A host-supplied observer of every recorded stop, called on the controller thread before the stop's action.
using ScriptStopFn = void (*)(void* user, u64 sequence);

struct InspectScript
{
    containers::StringView              file; // the name the program was loaded under: breakpoints are lines in it
    containers::ConstSpan<i64>          args;
    containers::ConstSpan<u32>          breaks;  // 1-based authored lines
    containers::ConstSpan<u32>          watches; // 1-based authored lines whose first result is read at every stop
    containers::ConstSpan<ScriptAction> actions; // one per stop, in order; Continue once they run out
    u32                                 max_stops = kScriptDefaultMaxStops;
    u32                                 wait_ms   = kScriptDefaultWaitMs;
    const std::atomic<bool>*            cancel    = nullptr; // the caller's flag (null: none)
    ScriptStopFn                        on_stop   = nullptr;
    void*                               user      = nullptr;
    HostRecording                       record{}; // DIAG.9a: record the execution (read it with InspectHost::record)
};

// NOLINTNEXTLINE(performance-enum-size)
enum class ScriptOutcome : u8
{
    NotStarted = 0,  // the host had nothing runnable, or the start was refused (`refusal`)
    Finished,        // the entry returned (`results`)
    Cancelled,       // the script cancelled it, or the stop bound did (`truncated`)
    CallerCancelled, // the caller's flag cancelled it
    Error,           // the run failed (`error`, at `fault_line`:`fault_col`)
    Unfinished,      // a wait ran out or the session refused the controller (`refusal`)
};
// "not-started", "finished", "cancelled", "caller-cancelled", "error", "unfinished".
[[nodiscard]] containers::StringView script_outcome_name(ScriptOutcome o) noexcept;

struct ScriptBind
{
    u32                 line   = 0U;
    inspect::BindStatus status = inspect::BindStatus::Bound;
    u32                 sites  = 0U;
};

struct ScriptStop
{
    u64                 sequence = 0U;
    inspect::StopReason reason   = inspect::StopReason::None;
    u32                 file_id  = 0U; // the stop's authored position (0s when the instr has none)
    u32                 line     = 0U;
    u32                 col      = 0U;
    u32                 depth    = 0U;
    u64                 op       = 0U;
    ScriptAction        action   = ScriptAction::Continue;
    u32                 first_value = 0U; // this stop's values are values[first_value, first_value + watches.size())
};

struct ScriptValue
{
    u32                  line     = 0U;
    bool                 answered = false; // false: the paused thread did not answer the snapshot request
    inspect::ValueStatus status   = inspect::ValueStatus::NoSuchValue;
    u32                  type_off = 0U; // the canonical type text in InspectReport::text
    u32                  type_len = 0U;
    bool                 has_unit = false;
    i64                  bits     = 0;
};

struct InspectReport
{
    explicit InspectReport(memory::IAllocator* alloc)
        : allocator(alloc), binds(alloc), stops(alloc), values(alloc), text(alloc), results(alloc)
    {
    }

    memory::IAllocator*             allocator; // the controller's: every array here and the value snapshots
    u64                             generation = 0U;
    containers::Array<ScriptBind>   binds; // one per script break, in order
    containers::Array<ScriptStop>   stops;
    containers::Array<ScriptValue>  values;
    containers::String              text; // type texts, concatenated
    bool                            truncated = false;
    ScriptOutcome                   outcome   = ScriptOutcome::NotStarted;
    inspect::Refusal                refusal   = inspect::Refusal::None;
    containers::Array<i64>          results;
    plan::RunError                  error      = plan::RunError::None;
    u32                             fault_line = 0U;
    u32                             fault_col  = 0U;

    [[nodiscard]] containers::StringView type_text(const ScriptValue& v) const noexcept
    {
        return containers::StringView{text.data() + v.type_off, v.type_len};
    }
};

// Run `script` on `host`, which must hold a loaded program and no attached execution. Lines are not validated here:
// a caller refuses a 0 line before it loads anything. On return the execution has ended and its thread is joined, or
// the outcome is Unfinished and the host's destructor cancels and joins it.
void run_inspect_script(InspectHost& host, const InspectScript& script, InspectReport& out);
} // namespace crd::ceir::cook
