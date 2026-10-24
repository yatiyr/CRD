# ADR-0133 — Runtime diagnostics and instrumentation

<!-- doc-role: decision -->
> Direction and acceptance contract. Live work: [ROADMAP](../ROADMAP.md#slice-diag.0).

**Status:** scope and immediate-after-REPO.DEV placement authorized by the user on 2026-09-14. The
[implementation contract](../design/runtime-diagnostics.md) defines required outcomes; exact provider/API choices
are resolved with evidence in DIAG.0 and owning leaves. No external vendor, new paid service, new runner or unavailable
hardware acquisition is approved by this ADR. RAH-0 and ADR-0107 remain separate unaccepted gates.

**Extends:** [ADR-0002](0002-logging.md), [ADR-0003](0003-memory-v1.md),
[ADR-0109](0109-ceir-chir-ckir-ownership-and-module-placement.md),
[ADR-0117](0117-compiler-infrastructure-skeleton.md),
[ADR-0120](0120-hot-reload-and-state-migration.md),
[ADR-0129](0129-renderer-ui-editor-delivery-order.md),
[ADR-0130](0130-system-qualification-and-agent-driven-products.md).

## Context

The [research](../research/2026-09-14-diagnostics-and-instrumentation.md) records useful existing sanitizer, profiler,
crash, log and GPU facilities alongside allocator blind spots, reported scheduler races, fragile fatal paths,
unqualified observer/registry lifetimes and incomplete source/capture integration. Building a larger renderer/editor
before a reliable diagnostic foundation would magnify investigation cost. Universal bug detection or effortless
Heisenbug diagnosis cannot be certified; observable bounded detection and reproducibility are the acceptance standard.

## Decisions

1. Insert **DIAG** immediately after REPO.DEV, before REPO.3d and renderer work, in the sole master table. Repair known
   free-list races and qualify the fiber race instrument early, before trusting later shared-state diagnostics.
   Preserve REPO publication gates and every retained renderer/UI/notebook obligation. This is planning authorization,
   not resumption of a stopped implementation loop.
2. Extend existing Cerid modules. Keep engine-facing profiling in crd-perf, ordinary logging in crd-log, allocation
   contracts in memory, lifecycle/scheduling in jobs and source provenance in CEIR/CKIR owners. Lower-layer hooks do
   not depend on UI or an IR executor. Hosts compose collectors/exporters through optional public services.
3. Separate bounded emergency recording from allocating/locking ordinary diagnostics. Prefer a healthy external
   crash collector where supported; the exact maintained backend or narrow OS implementation is chosen after
   licensing, platform and adversarial qualification. Reports must preserve actual failure and write results.
4. Make policies/triggers/experiments authorable through existing asset/configuration/execution services. Native
   adapters remain generic host mechanisms. Freeze validated policy before fatal execution; no parsing, allocation,
   CEIR execution or unsafe callbacks inside the emergency path.
5. Instrument each logical custom allocation and retain explicit lifetime/provenance. Combine sanitizer boundaries,
   structural invariants, optional guards/quarantine/sampling and generation-checked handles. Do not replace Cerid
   allocators with a system allocator or claim raw pointers become lifetime-safe after address reuse.
6. Preserve real causal ordering. Sanitizer annotations cannot invent happens-before merely to silence reports.
   Combine race detection, bounded schedule/weak-memory tests, multicore stress, wait graphs and rare-failure replay.
   Record limitations, including uninstrumented native code and GPU nondeterminism.
7. Correlate CPU/task/GPU/resource/command/program generation and source identity. Keep clocks distinct unless
   calibrated; report loss/uncertainty. Use versioned bounded capture formats and interoperable offline export.
   Future CR-D007 views consume these services rather than building a second profiler/debugger.
8. Default to local, bounded diagnostics with explicit restricted raw evidence. Upload, remote debug, arbitrary
   memory access and fault injection are separate capabilities. A raw dump is not sanitized merely because the
   accompanying manifest is redacted. Agent tools use the same typed authorization and budgets as human controls.
9. Qualify Windows/Linux first, design portable boundaries now, and retain explicit MAC.DIAG/WEB.DIAG release gates.
   No missing hardware or empty tool invocation counts as qualification. Exact platform tuples define support.
10. Use risk-selected local checks and CI for broader evidence. Every claimed detector has failing and valid controls,
    every hot-path mode has measured overhead, and every task updates the owning row/session and affected documents.

## Ownership transfer without lost requirements

DG05's immediate scheduler repair transfers from CORE-USE.2 into DIAG; CORE-USE retains current-renderer integration.
Early allocator/instrumentation/crash/trace mechanisms move forward from MEM-HARD/JOBS-HARD/OPS, while their later
product, peer-performance, distributed and new-platform qualification remains. SANITY-TLSF retains audits of later
consumers. RAH-6.b, LANG and editor inspectors extend the shared mechanisms in their original programmes. The
[contract](../design/runtime-diagnostics.md#existing-programme-ownership-after-diag) gives the exact boundary.

## Consequences and unresolved mechanism choices

The roadmap gains a finite diagnostic foundation before major renderer work. Research citations are guidance, not
installation decisions or performance claims. DIAG.0 resolves tool versions, schema migration and module placement;
child evidence resolves algorithm/provider choices within this authorized scope. Record these decisions here rather
than inventing hidden defaults. A real scope reduction, publication, paid service or changed product authority requires
the applicable user decision. Existing human-only commit/push and no-delegation rules remain unchanged.

## Implementation decisions

No implementation/provider selection or runtime qualification was performed by this planning change. Append numbered,
dated evidence-backed decisions here as the owning slices settle the mechanisms; status remains solely in ROADMAP.

### ID-1 (2026-09-14, DIAG.0) — schema name and migration

The diagnostic event, capture and bundle schema is **`cerid-diagnostics/1`**, in the repository's `cerid-<name>/N`
convention (alongside `cerid-ci-evidence/1`, `cerid-pins/1`, `cerid-build-bench/1`). It is a bounded, versioned,
POD-pinned format in the manner of [CPROF capture](../../engine/foundation/perf/include/crd/perf/capture.hpp): a
layout change bumps `N` and ships a reader for `N-1`; an offline export declares the schema version it wrote. This
resolves the "public schema compatibility" question the [contract](../design/runtime-diagnostics.md#diag-0) assigns to
DIAG.0. The unified crash+symbol+generation+capture bundle that consumes this schema remains `unsupported` today
(DG14) and is built by DIAG.5d/6c.

### ID-2 (2026-09-14, DIAG.0) — qualification tool tuples

DIAG qualifies against these exact tuples, recorded so later slices measure and detect against fixed versions:
GCC 13.3 (Linux, the sole ASan+UBSan-no-recover lane), Clang 22.1.3 (clang-cl lanes), MSVC 19.51 / Visual Studio 18
2026 (native), pinned LLVM 20.1.8 (clang-format, clang-tidy), CMake 4.3.2 local and 4.4.3 hosted. TSan and MSVC
use-after-return are **not** qualified in the presets (DG01); DIAG.1b and DIAG.3f own adding and qualifying them. No
new compiler, sanitizer or hardware target is asserted by this entry.

### ID-3 (2026-09-14, DIAG.0) — module placement

Concrete owners, extending existing modules per Decision 2 (no new top-level module): engine-facing profiling and the
capture/counter surfaces in **crd-perf** (`engine/foundation/perf`); ordinary logging in **crd-log**
(`engine/foundation/log`); allocation contracts and per-allocation instrumentation in **memory**
(`engine/foundation/memory`); lifecycle, scheduling and observers in **jobs** (`engine/foundation/jobs`); the fatal
path (crash and assert) in **crd-core** (`engine/foundation/core`); source provenance in the **CEIR/CKIR** owners;
GPU fault depth in the **gpu-context** backends; and the reusable per-host diagnostics lifecycle composed in
**crd-app** (`engine/foundation/app`). The **doctor** is a public query service whose interface lives in crd-perf (the
diagnostics-facing foundation module); each owning module contributes its mode status through a small registration
hook and hosts compose the report — no module gains a private profiler or a second crash path. The exact header home
of the doctor interface is settled in DIAG.0 increment two with the harness.

Amended 2026-10-12 (DIAG.11a, [session](../sessions/2026-10-12-diag-11a-reusable-diagnostic-host.md)): the reusable
per-host diagnostics lifecycle object, `crd::perf::DiagnosticHost`, lives in **crd-perf**, not crd-app. It owns the
flight recorder, the fault injector and their commands, which are crd-perf's, and a headless host such as ceridc links
neither crd-app nor crd-platform. crd-app composes it: `ApplicationDesc::diagnostics` hands a started host to the
Application, which then leaves the crash handler to it and feeds it frames. The crash path itself stays in crd-core;
the host only installs and uninstalls it.

### ID-4 (2026-09-14, DIAG.0) — provider choices within authorized scope

No external, paid or newly-installed crash/telemetry backend is selected; none is authorized by this ADR. The
qualified baseline is the in-tree path: on Windows the existing `MiniDumpWriteDump` route
([crash.cpp](../../engine/foundation/core/src/crash.cpp)) hardened to check its return and avoid second-resolution
filename collisions (DIAG.5a); on Linux `sigaction`/`backtrace` with `SA_ONSTACK` installed and install failures
reported (DIAG.5b). The emergency recorder is a bounded in-tree recorder, distinct from the allocating queued
[logger](../../engine/foundation/log/src/logger.cpp) (DG10). GPU fault providers are DX12 DRED and Vulkan device-fault
extraction, extending the existing [DX12](../../engine/gpu/gpu-context-dx12/src/dx12_validation_capture.cpp) and
[Vulkan](../../engine/gpu/gpu-context-vulkan/src/vulkan_validation_capture.cpp) validation capture (DIAG.7b/7c). A
healthy external collector (Decision 3) stays a separate future capability requiring its own user decision.

### ID-5 (2026-09-14, DIAG.0) — numeric budgets bound to named acceptance workloads

The [runtime-diagnostics targets](../design/runtime-diagnostics.md#modes-budgets-and-evidence-of-cost) are frozen here
as **engineering targets, not measurements**, each bound to a committed workload that DIAG.11b/11c will measure (with
variance and tails, recording the exact host tuple):

| Mode | Budget (target) | Acceptance workload |
|---|---|---|
| disabled | no event allocations, worker threads or exporter I/O; no regression statistically above 1% | perf zero-overhead gate ([test_zero_overhead_gate](../../tests/foundation/perf/test_zero_overhead_gate.cpp)) + jobs suite ([tests/foundation/jobs](../../tests/foundation/jobs)) |
| basic recording | ≤2% CPU/frame, ≤32 MiB process storage | jobs scheduler ([test_scheduler](../../tests/foundation/jobs/test_scheduler.cpp)) + a CEIR corpus replay (the [fuzz corpus CTests](../../tests/support/fuzz)) |
| emergency | ≤1 MiB default storage | the crash specimen harness (DIAG.0 increment two) |
| event payload | ≤256 bytes each, explicit truncation | perf capture round-trip ([test_capture_roundtrip](../../tests/foundation/perf/test_capture_roundtrip.cpp)) |
| bundle quota | ≤256 MiB and ≤10 bundles (excluding separately authorized full dumps) | the CPROF capture path ([capture.hpp](../../engine/foundation/perf/include/crd/perf/capture.hpp)) |
| hot recording | after registration: no allocation, blocking, symbolization, network I/O or global locking | perf zero-overhead gate + a KIR eval case ([test_ckir_asset](../../tests/gpu/kir/test_ckir_asset.cpp)) + an asset cook ([test_mesh_cook_options](../../tests/assets/cooker/test_mesh_cook_options.cpp)) |

Applications may author stricter budgets. Full dumps and GPU-assisted validation account their cost separately and are
not held to the basic-recorder budget. Evidence for these frozen numbers: the [capability census](../sessions/2026-09-14-diag-0-capability-census.md).

Amended 2026-10-16 (DIAG.11b, [session](../sessions/2026-10-16-diag-11b-overhead-harness.md),
[board](../bench/2026-10-16-diag-11b-overhead.md)): the budgets are unchanged and now measured by the overhead harness
(`tests/support/diag-overhead`) and `scripts/diag-overhead.py`. Its workloads deviate from the table above, on purpose:
the basic row's jobs scheduler is the harness's `jobs` workload (the crd-jobs pool configured by the diagnostic host);
its "CEIR corpus replay" is replaced by `offline`, which cooks, loads, compiles and runs an authored program, because
the corpora are mostly refused inputs and would measure the rejection path; allocation-heavy frames (`alloc`, a TLSF
heap the host's allocator watch observes) are the DIAG.11b contract's own workload with no row here; the disabled row's
zero-overhead gate and the hot-recording row's KIR eval and asset cook are not harness workloads, and a site
microbenchmark (`events`) reports each mode's per-site cost instead. The disabled budget is measured as the measured
binary's off mode against a binary built with profiling compiled out (`win-shipping-profile` against `win-shipping`);
Linux has no such preset pair yet. The first boards violate the disabled and basic budgets on allocation-heavy frames;
the repairs are open on DIAG.11b.

Amended 2026-10-23 (DIAG.11b, [session](../sessions/2026-10-23-diag-11b-realistic-disabled-cost.md)), by the user's
decision of 2026-10-19: `alloc` is the disabled budget's worst case, reported and not judged; the 1% disabled budget is
judged on the realistic workloads (`offline`, `jobs`, later the authored rendering workload). Where the two-binary
pair's layout floor is wider than the budget, a per-operation cost is judged by an exact count of its executions per
frame times its measured per-execution cost.

Amended 2026-10-24 (DIAG.11b, [session](../sessions/2026-10-24-diag-11b-jobs-cpu-per-frame.md)): each budget is judged
on the frame time and on the CPU per frame (every thread of the process over the timed frames), and the worse verdict
decides. The `jobs` workload runs a pool no larger than the physical cores its affinity leaves (four threads, the main
thread included, on eight cores, one logical processor each): an eight-thread pool on sixteen logical processors
spread one process's frame from another's by 7.6%, wider than either budget.

Amended 2026-10-24 (DIAG.11b, [session](../sessions/2026-10-24-diag-11b-instruction-counts.md)): the disabled budget
is also judged on the instructions executed per frame (`diag-overhead.py instructions`, Valgrind's cachegrind), which
do not depend on the binaries' layout; the simulated cache misses are reported. Linux measures it against a scratch
configuration of `linux-gcc-shipping` with profiling on; Windows, with no unprivileged counter on the reference host,
keeps the exact count times cost until DIAG.11c qualifies an instruction count.

Amended 2026-10-24 (DIAG.11b, [session](../sessions/2026-10-24-diag-11b-mixed-io-workload.md)): the contract's mixed
I/O workload is the harness's `io` (whole-file, range and `AsyncFile` reads of files it writes, on a two-thread pool),
and it joins `offline` and `jobs` as a realistic workload the disabled and basic budgets are judged on.

Amended 2026-10-24 (DIAG.11b, [session](../sessions/2026-10-24-diag-11b-authored-rendering-workload.md)): the
contract's authored rendering workload is the harness's `render` (the scene renderer through the authored
`engine://frame/forward_csm` graph on a headless device), a realistic workload too. It is judged on frame time and
CPU per frame only: on Linux the reference device is llvmpipe, whose rasteriser is the frame, so it is not counted
under cachegrind.

### ID-6 (2026-10-19, DIAG.11b) — investigation runbooks held to the tree

The contract's "symptom→command→artifact→next discriminating test and human-only publication" is
[docs/design/diagnostic-runbooks.md](../design/diagnostic-runbooks.md): one entry per symptom, each a table of fixed
fields (Symptom, Command, Artifact, Read, Next test, Hosted, Adopt, Example, Cost, Owner), entered from what is seen,
not from a tool. The entries are data a guard reads, not prose: `scripts/check-diag-runbooks.py` (the
`crd-check-diag-runbooks` CTest, run with each Ninja lane's tree) fails a command whose script, flag, lane command,
preset, `ceridc` verb, diag command, grant, executable or CMake name does not exist, a test regex that matches no test
the tree registers (rows marked `(Windows)` or `(Linux)` resolve on that host only), an artifact the sources never
write, an example path that is gone, a cost without a link to its measurement or its owning clause, and a DIAG child
that owns no entry and is not listed with its reason. Publication stays the user's: a runbook command may read hosted
evidence but never commits, pushes, releases, opens an issue or pull request, dispatches a workflow, writes through
`gh api` or grants the service's `upload` or `remote-enable` class. Each capability's Adopt row is the one step that
switches it on for a new module, test or application, so the renderer and execution rows ahead inherit them.
