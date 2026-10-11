# DIAG.10b last three owned gaps closed through test-only seams, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10b](../ROADMAP.md#slice-diag.10b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b); route:
> [test instruments](../design/test-instruments.md#diagnostic-coverage-and-the-nonreproduced-failure-register-diag10b).
> Rules: [AGENTS](../../AGENTS.md). Preceding: [twelve owned gaps closed](2026-10-11-diag-10b-owned-gaps-closed.md).

## Goal

The coverage registry still owned three error paths as `owned-gap`, each needing a seam in an engine header:
`PolicyLoadStatus::RejectedGenerationWrap`, `FlightPolicyStatus::RejectedGenerationWrap` and the flight recorder's
`kStateFatalEntered` transition on the fatal path that finds the ring mutex held. This batch adds the seams, a case
for each path, and removes the three exclusions. Committed locally, not pushed.

## The seams

All three are test-only and compiled only where `CRD_ENABLE_ASSERTS` is, never in a shipping build. This follows the
existing `crd::crash::test_fatal_path` and `test_inject_write_failure`. Both headers now include
`crd/core/build_config.hpp` so the gate is defined wherever they are seen.

| Seam | Header | What it does |
| --- | --- | --- |
| `DiagnosticPolicyStore::test_set_generation(g)` | `crd/perf/diagnostics.hpp` | continues counting from `g`; refuses 0 ("never loaded"); the active policy and the overflow flag stay |
| `FlightRecorder::test_set_policy_generation(g)` | `crd/perf/flight_recorder.hpp` | sets the policy generation under both mutexes; refuses 0 and a recorder before init |
| `FlightRecorder::test_with_ring_held(fn, user)` | `crd/perf/flight_recorder.hpp` | calls `fn(user)` while this thread holds the ring mutex; refuses a null `fn` and a recorder before init without calling it |

No seam returns a status itself: the producing sites the measurement names (`diagnostics.cpp`'s and
`flight_recorder.cpp`'s wrap guards, and the `state.store(kStateFatalEntered, ...)` in `finish`) are the engine's own.
The `GenerationKey` comment named a `next_generation()` that does not exist; it now names the store's wrap guard.

## The cases

All in crd-perf-tests, tagged `[diag10b]` (the flight cases also `[diag9b]`), inside `#if CRD_ENABLE_ASSERTS`.

| Gap | Case | What it asserts |
| --- | --- | --- |
| `PolicyLoadStatus::RejectedGenerationWrap` | `diag: a load that would wrap the generation is refused and keeps the last good policy` | the seam refuses 0; at 2^32 - 2 one more load commits at 2^32 - 1; the next valid load is refused, sets `generation_overflowed()`, and keeps the generation and the active policy; a bad version and a zero bound are still refused for what they are |
| `FlightPolicyStatus::RejectedGenerationWrap` | `flight policy: a reload that would wrap the generation is refused and keeps the ring` | the seam refuses before init and 0; one reload fits at 2^32 - 1; the next is refused with the policy kept; an out-of-bounds candidate is still RejectedOutOfBounds; the six events recorded before the refusal are the next capture's pre window (the ring was not reallocated); the guard still holds after re-arming |
| `kStateFatalEntered` (ring held) | `flight trigger: the fatal path that finds the ring held delivers the trigger alone and is terminal` | a crd-jobs worker (two threads, main not pumping) holds the ring through the seam while the test's thread runs `finish(Fatal)`. With nothing pending nothing is delivered and the recorder stays armed. With a trigger pending and two post events recorded: one delivery with `ring_unavailable`, `post_truncated`, end Fatal, no events, the trigger's provisional sequence 5, the policy generation and the replay recipe; then FatalEntered, nothing recorded, a later trigger Suppressed, no second delivery; the capture decodes from `encode_flight_record` |

A `std::mutex` cannot be try-locked by its owner, so the holder is a crd-jobs worker, not the test's thread. The
worker is always released, whatever the main thread saw, so it never outlives the case.

## Teeth

Seven breaks on win-debug, each an exact replacement in the engine source, rebuilt, run with `[diag10b]`, restored by
a fresh write (so the restored file recompiles) and rebuilt. Each failed one case:

| Break | Failed |
| --- | --- |
| T1 the policy store's wrap guard never fires | the policy-store case |
| T2 the store's seam ignores its value | the policy-store case |
| T3 the flight reload's wrap guard tests 0 instead of 2^32 - 1 | the flight generation case |
| T4 the fatal path with the ring held delivers nothing | the ring-held case |
| T5 that path re-arms instead of entering FatalEntered | the ring-held case |
| T6 that path does not set `ring_unavailable` | the ring-held case |
| T7 `test_with_ring_held` does not hold the ring | the ring-held case |

After the restore and rebuild, `[diag10b]` passed (4 cases, 88 assertions).

## Coverage, before and after

On `linux-clang-coverage` (WSL, clang 18.1.3) the measured programs were rebuilt and the measurement run once with
the old registry: 853 CTests passed (850 before, plus the three cases), the schedcheck program passed (47), 954 raw
profiles. The verdict was STALE with exactly three lines, one per closed gap, and no GAP. With the three exclusions
removed, the same export evaluates PASS, and `--validate` reports 63 scope files, 33 outcome sets, 20 exclusions.

| Measure | Before (last batch) | Now |
| --- | --- | --- |
| Regions | 9,210 / 10,356 | 9,235 / 10,372 |
| Branches | 5,967 / 7,634 | 5,989 / 7,644 |
| Functions | 1,022 / 1,071 | 1,025 / 1,074 |
| Lines | 17,107 / 19,662 | 17,163 / 19,691 |

The seams add 16 regions and three functions to the denominator. No `owned-gap` exclusion is left: 5 `windows-only`,
4 `dying-child`, 4 `default-value`, 3 `resource-failure`, 3 `other-configuration` and 1 `no-producer`.

## Verification

| Lane | crd-perf-tests |
| --- | --- |
| win-debug | whole tree built; 270 cases (267 + 3), 6,159 assertions |
| win-shipping | 134 cases (the seams and their cases compiled out) |
| win-clang-cl-shipping | 134 cases; linked first time |
| win-asan (inside vcvars) | 270 cases; the only ASan reports are the deliberate overflow and use-after-free specimens |
| linux-gcc-debug | 268 cases (265 + 3); `[diag10b]` repeated 3 times; no sanitizer report |
| linux-gcc-asan | 268 cases; `[diag10b]` repeated 3 times; the only reports are the four deliberate specimens (heap overflow, use after free, use after return, leak) |
| linux-clang-tsan (`setarch -R`) | 268 cases; `[diag10b]` repeated 10 times; no TSan report |

On win-debug the three new cases also ran through CTest, each as its own process, five times in a row with
`--repeat until-fail:5` (a local flakiness check only; CI retries nothing). The WSL lanes were built one at a time.

Repository checks: strict tidy (LLVM 20.1.8) clean on the six changed C++ files; clang-format on the added lines
differs only in the repository's hand alignment; the Allman check, test-diag-coverage, test-diag-seeded-faults,
`diag-coverage.py --validate`, the ctest guards, check-master-plan and check-repository pass.

## Remaining in DIAG.10b

The seeds on hosted lanes, and the hosted run the row names.
