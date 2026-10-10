#pragma once

// crd-ceir-cook -- minimizing the recorded inputs of a failing run (DIAG.9c).
//
// A plan record of a failed run (replay_record.hpp) holds the inputs that made it fail: the entry arguments and the
// host input reads (random draws, clock and step readings, packed events), in program order. minimize_record_inputs
// shrinks those values toward 0 while the run still fails the recorded way, and returns a new record of the smaller
// run, which `replay.run` reproduces like any other.
//
// The failure is the recorded outcome: the same run error, blamed on the same op (its stable id), with every host input
// read answered by the stream (a read the stream cannot answer would end the run InputUnavailable for another reason).
// A candidate counts only when every one of K runs fails that way (crd/core/minimize.hpp); the compiled-plan executor
// is deterministic, so K > 1 only guards the host's registrar and ops.
//
// Steps: the record's own inputs are judged first. The read stream is then cut to the reads the failing run made (a
// run that fails earlier reads less). Each argument, then each delivered read's value, is shrunk toward 0 by
// minimize_toward_zero (0 first, then the magnitude bisected). Passes repeat while a value moves, and a value is shrunk
// again only when another value moved after it was last shrunk (shrinking it again with nothing else changed would only
// drift along a local boundary). When the passes settle, every value has 0 not reproducing and its neighbour one step
// closer to 0 judged not to, given the final values of the others (a local boundary when the failure is not monotone
// in that value, as for a draw an op reduces modulo a bound).
// A read's position, kind and channel never change, nor whether the host delivered it: those are the program's read
// sequence, not its data. The new record is then made from one more run of the final inputs, through the same
// recorder a `replay.record` run uses, so its trace, outcome and read stream are that run's own.
//
// Refused before anything runs, like `replay.run`: a record that is not a plan record, one whose input stream is
// incomplete (it read more than a record keeps), one from another build (unless allowed), one missing an input its
// program needs, one whose program is not the content hash it was recorded with, and one whose program needs external
// I/O results (no build replays them, DIAG.9b). A record of a run that did not fail has nothing to minimize.
//
// Contract: docs/design/runtime-diagnostics.md (DIAG.9c); ADR-0133.

#include <crd/ceir/cook/hot_reload.hpp> // Registrar
#include <crd/ceir/cook/replay_record.hpp>

#include <crd/containers/string.hpp>
#include <crd/core/minimize.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <atomic>

namespace crd::ceir::cook
{

// NOLINTNEXTLINE(performance-enum-size)
enum class RecordMinimizeStatus : crd::u8
{
    Minimal = 0,   // the new record fails the recorded way and no single value can move closer to 0
    Budget,        // the run budget ran out; the new record fails the recorded way but is not claimed minimal
    NotReproduced, // the record's own inputs did not fail the recorded way in all K runs here
    NotAFailure,   // the recorded run did not fail: there is nothing to minimize
    Unsupported,   // not a plan record, or its host input stream is incomplete
    Refused,       // another build (unless allowed), a missing input, a program that is not the recorded content hash,
                   // or a program that needs external I/O results
    Unrunnable,    // the record's program does not load or compile
    Cancelled,     // the caller's cancel was raised
};

// "minimal", "budget", "not-reproduced", "not-a-failure", "unsupported", "refused", "unrunnable", "cancelled".
[[nodiscard]] containers::StringView record_minimize_status_name(RecordMinimizeStatus s) noexcept;

struct RecordMinimizeOptions
{
    minimize::Budget budget;            // K and the run bound
    bool             any_build = false; // minimize a record made by another build (as `replay.run build=any`)
};

struct RecordMinimizeResult
{
    explicit RecordMinimizeResult(memory::IAllocator* alloc) : reason(alloc) {}

    RecordMinimizeStatus status = RecordMinimizeStatus::NotReproduced;
    minimize::Stats      stats;
    crd::u32             args_changed  = 0U; // arguments whose value moved
    crd::u32             reads_changed = 0U; // kept reads whose value moved
    crd::u64             reads_cut     = 0U; // reads of the record the failing run no longer made
    containers::String   reason;             // why, for every status but Minimal and Budget
};

// Minimize `record`'s inputs. On Minimal or Budget, `out` is the new record (made by this build; the program, its
// path, asset, generation, entry, input states and event bound are the record's own); otherwise `out` is untouched.
void minimize_record_inputs(const ReplayRecord& record, Registrar registrar, void* user,
                            const RecordMinimizeOptions& options, const std::atomic<bool>* cancel, ReplayRecord& out,
                            RecordMinimizeResult& result);

} // namespace crd::ceir::cook
