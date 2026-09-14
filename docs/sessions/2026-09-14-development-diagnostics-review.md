# Development diagnostics review — 2026-09-14

<!-- doc-role: evidence -->
> Dated review, not another roadmap or implementation authorization. Owner: [REPO.DEV](../ROADMAP.md#slice-repo.dev).

## Scope and evidence

The user requested planning: explain how REPO improves development checks, dangling-pointer detection,
parallel-code diagnosis and profiling after recent changes. This task remains a reviewer; no implementation,
build, sanitizer experiment, scheduler change or automation operation was performed. HEAD was
`2b6dcdb090cea710fbaa7bdc4a94024ab5d2f510`; existing uncommitted CMake, preset, tooling, fuzz and documentation
repairs were preserved. Read the current orientation, roadmap, instrument/workflow contracts and relevant
memory/jobs/perf/app/GPU sources. Runtime results below are attributed to existing records, not rerun here.

Read-only GitHub inspection found [run 34821419392](https://github.com/yatiyr/CRD/actions/runs/34821419392)
still running, with failed completed jobs and other jobs pending. The
[first complete-tier repair record](2026-09-14-first-complete-tier-run-repairs.md) documents local repairs;
their presence in the worktree is not hosted qualification. This observation is dated evidence, not a live monitor.

## What the repository programme provides

- [REPO.DEV.3](../ROADMAP.md#slice-repo.dev.3): doctor/plan/check/evidence, target ownership and reverse consumers,
  scoped builds/tests/strict analysis, bounded execution and evidence tied to source/configuration identity.
  See [developer workflow](../design/developer-workflow.md).
- [REPO.DEV.4](../ROADMAP.md#slice-repo.dev.4), [DEV.6](../ROADMAP.md#slice-repo.dev.6),
  [DEV.7](../ROADMAP.md#slice-repo.dev.7), [DEV.8](../ROADMAP.md#slice-repo.dev.8): module dependency closure,
  pinned inputs, measured build/cache work, public-header/install/consumer qualification. These reduce rebuilds
  and environment/API surprises; they do not establish runtime memory safety.
- [REPO.DEV.5](../ROADMAP.md#slice-repo.dev.5): tier selection and required-result accounting. Source changes
  still run fourteen whole-preset lanes; affected-test sharding is not implemented merely because local selection
  exists. Diagnostic fuzzing stays local. See [CI tiers](../design/ci-tiers.md).
- [REPO.DEV.9](../ROADMAP.md#slice-repo.dev.9): CEIR/CKIR text/binary fuzz harnesses and bounded corpus replay,
  fail-fast UBSan and sanitizer-aware fiber switching. This is an ingestion baseline, not whole-engine fuzz coverage.

## Runtime maturity and remaining boundaries

**Memory.** ASan configurations exist on Windows/Linux; Linux combines ASan with UBSan. They exercise bad
accesses on executed paths, not every possible pointer lifetime. The
[instrument contract](../design/test-instruments.md) records a stack-use-after-return probe detected on GCC but
not MSVC with the current flags. Microsoft's
[documented MSVC mode](https://learn.microsoft.com/en-us/cpp/sanitizers/error-stack-use-after-return?view=msvc-170)
requires its additional compile option and runtime setting; this mode is not presently qualified by the presets.

[VirtualMemoryAllocator](../../engine/foundation/memory/src/virtual_memory_allocator.cpp) poisons its unused tail
and rewound memory for ASan. Equivalent per-slot lifetime poisoning was not found in the reviewed pool/TLSF/linear
allocators. [PoolAllocator::owns](../../engine/foundation/memory/src/allocators/pool_allocator.cpp) checks region
and slot alignment, not whether a slot remains allocated; TLSF's `owns` checks its region. Therefore a pointer
can pass ownership checks after its object is freed. This is a source-level diagnostic coverage finding, not a
new reproduced crash. Aggregate [profiler memory statistics](../../engine/foundation/perf/include/crd/perf/memory.hpp)
do not supply per-allocation allocation/free stacks. Existing owners:
[CORE-USE](../ROADMAP.md#slice-core-use), [MEM-HARD.1](../ROADMAP.md#slice-mem-hard.1),
[MEM-HARD.2](../ROADMAP.md#slice-mem-hard.2).

**Parallel execution.** The [fiber sanitizer adapter](../../engine/foundation/jobs/src/sanitizer_fibers.hpp)
models stack switches; `CRD_ENABLE_TSAN` exists, but has no qualified named preset/hosted lane. The instrument
record reports races on plain `next_free` links in the fiber and counter free lists, owned explicitly by
[CORE-USE.2](../ROADMAP.md#slice-core-use.2); the reviewed source still contains those fields. ASan passing cannot
close that finding. TSan detects data races; deadlocks, starvation, cancellation and wrong completion order also
need specific lifecycle/interleaving/timeout tests. The
[observer end-hook repair](2026-09-13-jobs-end-hook-ordering.md) is a concrete example: completion had allowed
`wait` to return before the observer ended. The latest repair record reports hosted jobs-suite passes for its fix.
Broad ownership/fairness/soak qualification remains under [JOBS-HARD](../ROADMAP.md#slice-jobs-hard).

**Profiling.** [crd-perf](../../engine/foundation/perf/include/crd/perf/profiler.hpp) already provides CPU regions,
frame history, counters and a [job adapter](../../engine/foundation/perf/include/crd/perf/jobs_adapter.hpp).
[perf-ui](../../engine/ui/perf-ui/include/crd/perf/ui/profiler_panel.hpp) provides a diagnostic ImGui panel and
capture support. The [sandbox](../../sandbox/src/main.cpp) initializes profiling, installs the jobs adapter,
registers the default allocator and marks frames. The application base does not establish this profiling lifecycle
for every application; custom allocators require registration.

DX12/Vulkan implement actual per-pass timestamp queries through
[IFrameGraph](../../engine/gpu/gpu-context/include/crd/gpu/frame_graph.hpp), and the sandbox reads them.
However, searching engine/sandbox/tests found only a mock implementation of
[IProfilerGpuBackend](../../engine/foundation/perf/include/crd/perf/gpu_scope.hpp), with no production call to
`set_gpu_backend`. That header's example still references retired `crd-rhi-vulkan`/`crd::rhi` names. Consequently
GPU timing support must not be presented as a completed shared CPU/job/GPU timeline. Integration/attribution
belongs with [OPS.1](../ROADMAP.md#slice-ops.1), [D7E-7](../ROADMAP.md#slice-d7e-7) and
[D7E-10](../ROADMAP.md#slice-d7e-10); public examples need correction when that integration is settled.

**Crash diagnosis.** [ApplicationDesc](../../engine/foundation/app/include/crd/app/application.hpp) enables
the [crash handler](../../engine/foundation/core/include/crd/core/crash.hpp) by default: Windows minidumps;
Linux signal/backtrace logging and re-raising for core handling. This is useful existing infrastructure. A unified
failure bundle with build/symbol identity, recent CPU/job/GPU events, assets/programs, reload generation and
reproduction inputs remains the [OPS.1](../ROADMAP.md#slice-ops.1) contract, currently much later in the sequence.

## Planning recommendation, not an accepted reorder

Close existing REPO qualification first. Then propose a bounded diagnostics baseline before major renderer/UI
implementation, using the existing owners rather than a second tracking document:

1. Through CORE-USE, resolve the known free-list races and prove scratch/counter/callback ownership. Qualify an
   on-demand race-checking route plus seeded negative controls; add it to CI on suitable changes/periodically.
2. Bring forward the necessary subset of MEM-HARD: allocator-aware checks for freed/invalid slots, diagnostic
   allocation/free provenance, explicit borrowed lifetimes and generation-checked handles at reloadable boundaries.
   Tests must deliberately misuse each supported allocator and verify detection, including release/reuse limits.
3. Bring forward the foundational part of OPS.1: lifecycle-managed application diagnostics, common CPU/job/GPU
   attribution and failure bundles, usable before CR-D007 exists. Keep the future crd-ui viewer a consumer.
4. Measure diagnostic overhead and enable tools by risk: one primary local configuration, affected consumers,
   an ASan/UBSan route for lifetime/ingestion work, a separate TSan route for shared-state work, optimized captures
   for performance. Preserve broader CI qualification without requiring every local edit to build the full matrix.

Acceptance should include intentionally failing specimens (use-after-free/return, race, hung job, reload with
pending work, GPU hazard), a nonzero executed check and a useful symbolized report. This is a recommendation
requiring a roadmap-order decision; no new slices, statuses or order changes were made during the review.

Tool limits are documented by primary sources:
[LLVM ASan](https://clang.llvm.org/docs/AddressSanitizer.html),
[LLVM TSan](https://clang.llvm.org/docs/ThreadSanitizer.html). Instrumentation is diagnostic evidence, not proof
that arbitrary application code is free of lifetime/concurrency bugs.

## Close-out

Added this review and its REPO.DEV reference; corrected the stale next-push wording/handoff in context and the
REPO.DEV close description. Retained all pending qualification and the existing current-slice pointer.
No source changes or new benchmark claims. `check-master-plan.py` PASS (863 rows, 1,054 documents, 9,212 local
links); `git diff --check` PASS. Implementation verification remains the linked existing evidence.
