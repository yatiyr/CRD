# DIAG.12: out of memory during log formatting, 2026-10-25

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12); clause owner:
> [DIAG.5c](../ROADMAP.md#slice-diag.5c). Contract: [runtime diagnostics](../design/runtime-diagnostics.md#diag-5c),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [the re-read, source half](2026-10-25-diag-12-re-read.md).

## CI read

No completed run has a failed job. 40122902896 (head `a4500838`) is still running; its four failed jobs are the two
causes `7bee95e8` and `17176125` fixed. The cancelled runs 40125506582 to 40131739936 have no failed job.

## The clause

DIAG.5c's acceptance names "OOM during formatting". The row declared two outcomes: a throwing allocator is a counted
drop through `dispatch`'s catch, and a null one ends in a `String::grow_to` null dereference. Neither held in final
source:

- `CRD_LOG_*` called `std::format` in the caller, before `dispatch`, so a throw from formatting escaped into the
  logging caller. In a `noexcept` caller (the assert bridge is one) that is `std::terminate` with no evidence.
- `String` growth ends in `allocate_or_fatal`, and the heap allocator is fatal on its own: out of memory is a
  `CRD_FATAL`, never a null dereference.

## Change

- `CRD_LOG_IMPL` calls `detail::dispatch_format`, which checks the format string at compile time and passes
  `std::make_format_args` to the out-of-line `detail::vdispatch` (`engine/foundation/log`). `vdispatch` formats into
  a crd `String` and dispatches; a throw is counted in `dropped_count()`. The caller no longer builds a `std::string`.
- Declared outcome: a formatter that throws drops that record, counted. Out of memory while formatting is the
  String's or heap allocator's out-of-memory fatal. Inside a sink, with the sink lock held, the bridge suppresses its
  own record and counts it (`sink_reentrant_assert_count()`), and the fatal ends the process.
- `crd-log-fatal-specimen` (`tests/support/diag/specimens/log_fatal_specimen.cpp`, a new file: no specimen under the
  expected-fatal protocol linked crd-log) has a sink that formats each record into a String on a refusing allocator,
  as the engine's sinks format. Modes `sync-sink-format-refused` and `async-sink-format-refused` pass only when the
  String's fatal arrives after the bridge counted one suppression.
- `docs/log/LOG_FILE.md` describes the new macro path.
- Naming `log_macros.hpp` to tidy-files showed nine `cppcoreguidelines-macro-usage` findings, there since the macros
  were written. A function evaluates its arguments before it can check the level, and only a macro captures the
  caller's `source_location` and compiles out whole, so the block carries a reasoned `NOLINTBEGIN`, as
  `asan_poison.hpp` and `sched_check.hpp` do.

## Teeth

- The old macro body (`std::format` in the caller) fails the case `a formatter that throws drops that record,
  counted, and never throws into the logging caller`: the exception reaches the caller and nothing is counted.
- With the bridge's in-sink check disabled, win-debug fails both modes with `CRD_DIAG_FATAL_OTHER` (MSVC refuses the
  recursive lock, so the bridge's record is dropped, not suppressed) and linux-gcc-debug times out both (the thread
  deadlocks on its own sink mutex). Restored, rebuilt, all pass.

## Verification

- win-debug: crd-log-tests (34 cases) and the two modes pass.
- linux-gcc-debug (`wsl-lane.sh`): the same targets build; the formatter case and both modes pass.
- Guards, tidy-files, clang-format (no more findings than HEAD), Allman, check-master-plan and check-repository pass.

Hosted once pushed: the formatter case and the two `crd-log-fatal-*` modes pass on every lane. The macro is in every
logging unit, so every lane's build is the evidence for its call sites.
