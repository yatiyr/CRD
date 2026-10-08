#pragma once

// crd-ceir-cook (private) -- what a replay of one program needs, from each op's effective effects. `replay.prepare`
// answers it for a program file, and `replay.record` stores it in a run record so `replay.run` can refuse a record
// that lacks an input its program needs.
//
// One input per slot, in a fixed order, each tagged with the replay guarantee it serves (identity, event, schedule,
// numeric). An input with no effect families is needed by every run. An effect-derived input is needed when a
// registered op's effective effects (a func.call's are its callee's, resolved through the module's symbol table, with a
// cycle guard cleared per op) include one of its families; it is unknown when no op needs it but an opaque op exists
// (unregistered, or with ExternalCall effects), and otherwise not needed.

#include <crd/ceir/cook/replay_record.hpp> // ReplayNeed, kReplayInputs
#include <crd/ceir/ir.hpp>
#include <crd/ceir/semantics.hpp> // DeterminismClass
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>

namespace crd::ceir
{
class Context;
} // namespace crd::ceir

namespace crd::ceir::cook::detail
{
// One replay input. `families` 0: the input is needed by every run, whatever its ops do.
struct ReplayInputSpec
{
    containers::StringView name;
    containers::StringView guarantee;
    crd::u64               families = 0U;
    containers::StringView what;    // the evidence, for the reasons
    containers::StringView missing; // why it is missing when needed and nothing holds it
};

inline constexpr crd::u32 kReplayInputCount = kReplayInputs;

// program, build, entry-arguments, random, clock, host-state, external-results, schedule, device-tolerance.
[[nodiscard]] const ReplayInputSpec& replay_input(crd::u32 index) noexcept;

inline constexpr crd::u32 kReplayProgramInput         = 0U;
inline constexpr crd::u32 kReplayBuildInput           = 1U;
inline constexpr crd::u32 kReplayArgumentsInput       = 2U;
inline constexpr crd::u32 kReplayRandomInput          = 3U;
inline constexpr crd::u32 kReplayClockInput           = 4U;
inline constexpr crd::u32 kReplayHostStateInput       = 5U;
inline constexpr crd::u32 kReplayScheduleInput        = 7U;
inline constexpr crd::u32 kReplayDeviceToleranceInput = 8U;

using Need = ReplayNeed;

// Whether input `index` is one the host input seam carries (random, clock, and host-state through input.event): every
// read of it goes through an InputRecorder, so a record holds it when the run kept every read and no op reads it
// outside the seam (a scene, ECS, physics, audio, document, constraint or other UI read keeps host-state missing).
[[nodiscard]] constexpr bool is_seam_input(crd::u32 index) noexcept
{
    return index == kReplayRandomInput || index == kReplayClockInput || index == kReplayHostStateInput;
}

// "no", "yes", "unknown".
[[nodiscard]] containers::StringView need_name(Need n) noexcept;

// "none", "bit-exact", "within-target", "within-backend", "nondeterministic", "external-nondeterminism".
[[nodiscard]] containers::StringView claim_name(DeterminismClass c) noexcept;

struct InputNeed
{
    Need             need  = Need::Yes;
    crd::u64         ops   = 0U;      // ops whose effective effects need it
    const Operation* cause = nullptr; // the first of them in pre-order (the first opaque op when Unknown)
    // DIAG.9a: ops that declare one of its families themselves and are not host input reads (input.hpp), so nothing
    // at the input seam sees what they read. A call is charged with its callee's effects but declares none itself.
    crd::u64 uncaptured = 0U;
};

struct ProgramNeeds
{
    crd::u64         ops          = 0U;
    crd::u64         unregistered = 0U;
    crd::u64         opaque       = 0U; // unregistered, or effective effects including ExternalCall
    const Operation* first_opaque = nullptr;
    crd::u64         unclaimed    = 0U; // registered ops that make no determinism claim
    DeterminismClass weakest      = DeterminismClass::Unspecified; // weakest claim made (Unspecified: none made)
    InputNeed        inputs[kReplayInputCount];
};

// Classify every op of `module` in pre-order. Returns false, with `out` incomplete, when `cancel` was raised during
// the walk (it is polled once every 256 ops).
[[nodiscard]] bool analyze_needs(const Context& ctx, const Module& module, memory::IAllocator* alloc,
                                 const std::atomic<bool>* cancel, ProgramNeeds& out);

// A run record's input states from `needs`: the program, its build and its entry arguments are recorded, and so is
// the schedule when the host executor ran it (the record holds its settings); random streams are recorded when
// `host_inputs_held` (the record holds every host input read of the run) and only input ops read randomness
// (`uncaptured` is 0); any other input the program needs (or may need, through an opaque op) is missing, because
// nothing captures it at the executor's boundary; the rest are not needed. `missing` (when not null) gains the missing
// inputs' names, comma-separated, in record order.
void record_inputs(const ProgramNeeds& needs, ReplayExecutorKind executor, bool host_inputs_held,
                   ReplayInput (&inputs)[kReplayInputCount], containers::String* missing);
} // namespace crd::ceir::cook::detail
