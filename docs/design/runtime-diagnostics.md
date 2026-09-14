# Runtime diagnostics implementation contract

<!-- doc-role: contract -->
> Acceptance and implementation reference. Only [ROADMAP](../ROADMAP.md#slice-diag.0) contains slices/status.
> Read [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md), then the
> [source findings and primary research](../research/2026-09-14-diagnostics-and-instrumentation.md).

## Scope, execution and ownership

Implementation entry: query the exact row with `python scripts/check-master-plan.py --slice <ID>`, read its section
below and the linked DG source finding, then refresh the production consumer. State the full failing/valid controls
before coding. Finish the row's code, tests, session and affected docs together; record CI evidence in ROADMAP.
This file's proposed interfaces and modes are requirements until their rows qualify, not existing helper commands.

The DIAG block immediately follows REPO.DEV and precedes REPO.3d and renderer work. It implements the diagnostic
foundation for existing modules and real Windows/Linux consumers. All leaves are sequential in ROADMAP; this file
defines their deliverables, not a parallel checklist. Preserve earlier CI gates. No implementation is authorized
merely by an old loop grant. Geometry/physics and unrelated product algorithms remain outside this programme.

Extend core, memory, jobs, log, perf, app, resources, scene and CEIR/CKIR/provider seams as their owners require.
Use [DG01–DG18's source links](../research/2026-09-14-diagnostics-and-instrumentation.md#source-findings-and-owners)
as the initial reuse census. Refresh it before editing. Lower modules emit through small dependency-safe hooks or
records; they must not depend on the application, UI, an exporter, or a CEIR executor merely to report a failure.
Compose services at the host layer. Keep external provider types out of public APIs and retain modular headless use.

crd-perf is the owned profiling service. crd-log remains ordinary logging. Existing CEIR diagnostic codes, locations,
stable semantic IDs, executor hooks and resource generations are reused. A bounded emergency recorder is a separate
failure path because normal logging/capture allocates and locks. It is not a second general logger or scheduler.
Allocator diagnostics remain inside/alongside Cerid allocators; replacing them all with malloc would hide the target.

Diagnostic policies, collection recipes, trigger definitions and experiments are versioned authorable assets using
existing config/resource/CEIR paths. Native crash, clock, memory-protection, debugger and sampling adapters are generic
host mechanisms. Validate/compile policies while healthy; never parse, allocate, execute CEIR or hot-reload code in
a fatal signal handler. Last-known-good policy remains installed on invalid reload. Fault injection is an explicitly
privileged test facility, disabled in ordinary shipping hosts.

## Shared acceptance rules

Every child supplies a real production-path positive case, a deliberately failing specimen for its claimed detection
class, and a regression preventing its own instrument failure. Run dangerous specimens only in isolated child
processes with time/resource bounds. The parent test succeeds only when the expected diagnostic code, source/identity,
exit reason and artifact are observed; an arbitrary crash, missing executable, zero tests or timeout is not success.
Do not put intentionally undefined behaviour in ordinary in-process unit tests.

Record OS/ISA/compiler/runtime/API/adapter/driver, optimization flags, sanitizer options, asset/policy hashes,
instrument activation, selected and executed counts, original exit/signal, missing events, budgets and raw artifacts.
Validate the failing specimen without relying on tests that merely mirror implementation. Preserve first failure
evidence through retries. Quarantine or a clean rerun does not establish root cause; unknown-origin historical
failures remain labelled unknown until explained or bounded by explicit evidence.

Use one primary local configuration, affected consumers and incremental LLVM-20 tidy for changed C++. Add only
the diagnostic route relevant to the risk. Broad qualification runs in CI. Do not run all configurations locally.
Current tool availability is in [BUILDING](../BUILDING.md), [workflow](developer-workflow.md) and
[test instruments](test-instruments.md); route names introduced below are deliverables, not commands that exist today.

## Event, capture and failure schema

Define versioned records carrying: diagnostic code/severity/domain, stable event ID and sequence, process instance,
logical task/fiber/worker, thread, command/transaction, asset/program content hash plus generation, source/node origin,
resource/allocation generation, queue/submission/fence, monotonic clock domain/tick and uncertainty, typed payload,
redaction class and capture policy identity. Unknown fields have explicit unavailable reasons; zero is not silently
interpreted as measured time or successful execution. IDs must survive export without exposing raw pointers as identity.

Use bounded dictionaries with owned strings or immutable interned generations. IDs cannot silently alias when a
thread, fiber, allocation, DLL, source buffer or GPU object is reused. Preserve deleted generation metadata until
all trace/capture readers retire. Durations have explicit units and separate CPU/GPU/time-domain meaning.

A failure bundle contains a versioned manifest, binary/symbol/module identities, configuration and capability tuple,
raw fault record, recent bounded events/logs, job/wait summary, allocator/resource pressure, available GPU evidence,
asset/program/source-map identities and a replay recipe or a precise reason replay is unavailable. Individual sections
are independently bounded/checksummed with complete/truncated/unavailable states. Keep restricted raw dump material
separate from a shareable structured summary. Import never executes attached commands, plugins or programs.

## Modes, budgets and evidence of cost

Require disabled, basic-flight-recorder, full-profile, memory-check, race-check and controlled-replay modes. Some
instrument choices require a different binary or startup; the UI/CLI must say so. Requesting unavailable coverage
fails explicitly. ASan and TSan run separately. Expensive GPU validation/reverse debugging are targeted modes.

DIAG.0 must freeze numeric budgets for the acceptance workloads before implementation. Starting engineering targets
(not measurements or permission to silently weaken requirements): disabled mode has no event allocations, worker
threads or exporter I/O and no statistically resolved regression above 1%; basic recording targets <=2% CPU/frame
cost and <=32 MiB process storage; default emergency storage <=1 MiB; each event payload <=256 bytes with explicit
truncation; default local bundle quota <=256 MiB and 10 bundles, excluding separately authorized full dumps. Allow
application-authored stricter budgets. Measure variance and tails: a noisy run cannot qualify a 1% claim.

Hot recording after registration does no allocation, blocking, symbolization, network I/O or global locking.
Disabled event sites must not evaluate expensive payload expressions or change application semantics. Verify this
with side-effect/cost controls; performance-sensitive code cannot require logging to establish synchronization.
Overflow drops according to declared priority, counts loss and preserves a reserved fault channel. Per-thread/fiber
buffers have a process-wide cap; registering more threads cannot evade it. Large offline/headless runs need bounded
streaming/chunk rotation, never unbounded in-memory traces. Device validation and sanitizers report their measured
cost separately; they are not required to fit the basic recorder budget.

<a id="diag-0"></a>
## DIAG.0 — capability census and acceptance fixtures

Refresh DG01–DG18 against source and actual consumers. Enumerate every allocator/container owner, lifecycle callback,
executor/provider and diagnostic path; classify implemented, tested, unqualified and unsupported in dated evidence.
Record existing current hardware without adding paid services or registering runners. Resolve module placement,
public schema compatibility, diagnostic provider choices and numeric workloads/budgets in ADR-0133's implementation
notes. Build tiny isolated specimens and a harness that distinguishes expected detection from instrument failure.

Acceptance: doctor reports compiled/enabled/usable modes and dependencies; intentionally missing symbolizer, wrong
sanitizer runtime, zero test selection, denied output and mismatched binary are detected. No source-runtime failure
claim is invented from the census. The rest of DIAG inherits this fixture manifest and budgets.

<a id="diag-1a"></a>
## DIAG.1a — known scheduler free-list races

This is the primary repair owner of DG05, formerly CORE-USE.2: plain `next_free` reads/writes in fiber and counter
free lists. Establish an independent failing case, fix the actual C++ memory-order/reclamation mechanism and document
the linearization/ABA argument. Atomic links are a candidate, not permission to assume all reuse/ownership is solved.

Acceptance: original TSan reports disappear with no suppression; adversarial acquire/release exhaustion/reuse,
nested waits and counter reclamation preserve uniqueness and completion. Run real multithreaded consumers and check
both instrumented and production paths. CORE-USE.2 later verifies renderer integration, not a duplicate repair queue.

<a id="diag-1b"></a>
## DIAG.1b — trustworthy TSan and fiber model

Qualify a named Linux race-checking route and CI scope. Review every fiber create/switch/destroy, queue publication,
stack reuse and instrument-only fence substitution. Use `no_sync` only where the real happens-before contract demands
it; neither flag is correct by decree. Preserve unsynchronized negative-control races across fibers and migration.

Acceptance: known ordered specimens pass and unordered siblings fail; raw thread and fiber versions agree. Record
compiler/runtime and any tool workarounds per process. The dated WSL mapping workaround is not a general ASLR policy.
Instrumentation cannot add ordering merely to obtain green results. Driver/vendor uninstrumented boundaries stay explicit.

<a id="diag-2a"></a>
## DIAG.2a — typed events, identities and policy

Implement the shared schema, severity/fatal distinction and authorable policy with bounds and last-known-good reload.
Bridge existing CEIR diagnostic codes without forcing memory/jobs/core to depend upward on CEIR. Define owned strings,
symbol/source identities, monotonic clocks and stable generation keys. Separate recoverable errors, developer assertions,
instrument failures and fatal invariant violations; required runtime validation must not disappear with debug asserts.

Acceptance: C++ and an existing CEIR consumer emit the same structured failure context through CLI/JSON and logging;
malformed/oversized policy cannot change the active generation. Unknown versions, ID reuse/wrap and truncation are explicit.

<a id="diag-2b"></a>
## DIAG.2b — lifecycle and bounded emergency recording

Implement preallocated event storage and the independent minimal emergency record. Registration returns a lifetime
token or equivalent lifecycle-owned object; deregistration waits for or safely retires existing readers, including
dynamic labels, allocator entries, jobs observers and DLL callbacks. Specify reentrancy and early-init/late-shutdown
behaviour. Investigate DG08's unsynchronized registry reads rather than assuming its writer mutex protects readers.

Acceptance: concurrent registration/snapshot/deregistration/destruction, capture during shutdown, thread churn,
reentrant failure and full buffers retain valid records without dangling pointers. A crash while the ordinary log
mutex is held still yields an emergency record. Test under TSan once DIAG.1b is qualified; retain that evidence gate.
DIAG.1b is a prerequisite in the sequence; do not defer this concurrency evidence to a later child.

<a id="diag-3a"></a>
## DIAG.3a — complete allocator contracts

Inventory malloc, linear/growable-linear, stack, pool/growable-pool, TLSF/growable-TLSF, virtual-memory, ring, offset,
streaming/category and thread-safe wrappers, plus backing/chunk allocators discovered in the census. Declare owner,
alignment, zero-size, overflow, exhaustion, reset/rewind, reallocation and concurrency semantics for each. Offset/GPU
allocators validate logical ranges/lifetimes; do not apply CPU ASan poisoning to an unmapped device address.

Acceptance: checked size/stride/add/multiply/alignment/page arithmetic, failure preserving previous allocation,
wrong-allocator/free ownership, null and near-address-width boundaries. Cover parent/child arena destruction, external
buffers and partial construction. Preserve production layout/performance where diagnostic metadata can be side storage.

<a id="diag-3b"></a>
## DIAG.3b — allocator-aware sanitizer boundaries

Extend the existing ASan adapter to each addressable allocation and container live range. Correctly model reserve,
commit, padding, allocate/free, shrink/grow, reset, rewind, decommit and pool reuse; account for shadow granularity.
Keep free-list bookkeeping accessible without leaving the entire freed payload unpoisoned. Separate external backing
ownership and nested allocator annotations; annotations must not accidentally unpoison a parent's protected range.

Acceptance: one-byte under/overrun, freed-slot access, rewind use, unused container capacity and small/over-aligned
objects are intentionally detected where declared. Test large/odd-sized slots, metadata manipulation and non-ASan
builds. Immediate same-address reuse remains a documented raw-pointer limit, addressed by DIAG.3e.

<a id="diag-3c"></a>
## DIAG.3c — provenance, guards and sampled field diagnostics

Add optional allocation/free stack IDs, size/alignment/allocator/owner/task/generation, redzones, bounded quarantine,
guarded sampling and leak/retention summaries. Capture PCs cheaply and symbolize offline. Do not recurse into the
allocator being diagnosed; metadata uses a bounded independent arena with explicit exhaustion. A sampled mode must
report sample probability, eligible sizes and retained history instead of implying exhaustive detection.

Acceptance: report the allocation and free sites of an intentional stale access; guard-page under/overruns and
quarantine exhaustion are isolated and attributable. Prove metadata saturation does not corrupt the allocator or
silently disable a requested mandatory mode. Measure memory/throughput/tail cost versus uninstrumented allocators.

<a id="diag-3d"></a>
## DIAG.3d — structural allocator integrity and failure recovery

Implement debug structural walkers for free-list membership, duplicate/cyclic links, block adjacency, free-bit maps,
pool slot state and accounting consistency. Use an independent allocation-state model in randomized operation sequences.
Include checked OOM and realloc failure atomicity, fragmentation, cross-thread free where supported, and invalid-free
rejection before allocator metadata mutation. Audit heavy-churn growable TLSF init_pool invariants from SANITY-TLSF.

Acceptance: seeded metadata corruption and double/interior/wrong-owner frees give named failures, not hangs or silent
success. Random allocate/reallocate/free/reset sequences compare live ranges/data with the model and minimize failures.
Cover all current heavy-churn consumers; later newly introduced consumers inherit SANITY-TLSF requalification.

<a id="diag-3e"></a>
## DIAG.3e — handles, borrows and asynchronous lifetime

Audit existing resource handles, scene slot maps, intrusive references, callbacks, spans/string views and container
invalidation before adding types. Apply generation-checked identities at reloadable/public boundaries; define wrap,
ABA, destruction, reference-cycle and ownership-transfer policy. Never promise that `owns()` validates a raw object's
liveness. Make suspended work retain owners or fail a checked borrow contract; no blanket shared ownership everywhere.

Acceptance: use after entity/resource destruction, same-slot reuse, borrow after resize/reset, owner destruction while
a job is suspended, callback after unsubscribe and DLL reload with pending work. Prove C++ and existing CEIR consumers
follow the same lifetime; defer only integration with not-yet-existing CHIR/product hosts to their explicit owner rows.

<a id="diag-3f"></a>
## DIAG.3f — qualified memory, initialization and leak routes

Qualify Windows/Linux ASan+appropriate UBSan, MSVC use-after-return, use-after-scope where supported, allocator
annotations and fiber stack retirement. Add a bounded initialized-memory route: a coherently instrumented MSan closure
or qualified Memcheck pool support on Linux, with explicit coverage limitations. Leak reports distinguish retained
arenas, intentional caches and unreachable live objects. Add scoped static lifetime/ownership checks supported by the
pinned compiler; do not infer newer LLVM features are present in LLVM 20.

Acceptance: every claimed error class has a seeded subprocess detector test, symbolized site and nonzero failure.
Partial-instrumentation dependencies, LTO/optimization, CRT/DLL boundaries and tool absence are explicit. A missing
route remains unqualified; no broad warning suppression or conversion of a diagnostic failure into a skip-pass.

<a id="diag-4a"></a>
## DIAG.4a — structured job and observer lifetime

Complete current scheduler counter/fence/callback retirement and scratch ownership contracts. Record unique task
instances separately from recycled fiber addresses; retain causal parent/publication/resume/end/cancel edges. Make
observer replacement safe or enforce quiescence as an explicit checked API contract. Cancellation must not release
owners while native/GPU/I/O callbacks can still run; establish drain semantics and no-blocking contexts.

Acceptance: one worker, nested jobs, migration, pool exhaustion, shutdown with pending work, cancellation at each
transition, observer unload/replacement and completion-before-wait-return ordering. Negative controls expose each
broken invariant; tests prove actual pooled execution rather than a sequential fallback.

<a id="diag-4b"></a>
## DIAG.4b — controlled interleavings and weak-memory checks

Add test-only scheduling choice hooks to the existing job system, with seed, choice log, bounds, replay and minimized
failure sequence. Exercise production state transitions; do not build a second product scheduler. Extract minimal
atomic algorithm models for a maintained model checker such as GenMC, pinning tooling separately if required.

Acceptance: a deliberately broken publication/reclamation algorithm is found and replayed; the repaired bounded
model passes with bounds stated. Run multicore stress without controlled scheduling too. Distinguish logical schedule
replay, weak-memory model results and real hardware execution; none implies universal exploration of the others.

<a id="diag-4c"></a>
## DIAG.4c — hangs, starvation and real-time violations

Implement logical wait graphs, task progress/deadline records, pool/queue saturation and lock/owner diagnostics.
Provide a watchdog outside the affected pool and an optional external process observer. Capture worker stacks plus
parked-fiber/task state using a safe stop/snapshot protocol, never walking changing stacks unsafely. Add portable
no-allocation/no-blocking sentinels for declared real-time callbacks; newer RTSan is optional after tool qualification.

Acceptance: seeded deadlock, livelock, priority starvation, pool exhaustion and forbidden blocking/allocation yield
distinct reports. Long progressing offline work, debugger pauses, suspend/resume and clock changes do not trigger a
false deadlock claim. Unresponsive processes have a bounded snapshot attempt and an honest incomplete result.

<a id="diag-5a"></a>
## DIAG.5a — Windows crash and hang capture

Replace the unreliable fatal-path dependency on in-process dumping with a qualified external handler or OS-supported
mechanism behind the owned crash API. Keep a minimal emergency fallback. Check every install/write result, preserve
exception context, serialize DbgHelp access, use Unicode/long-path-safe and collision-safe files, and support early
startup, native plugin faults, stack overflow and fail-fast limitations. No automatic upload.

Acceptance: crash while allocating/holding a logger lock, concurrent crashes, handler failure, denied/full output,
missing symbols and stack exhaustion. A successful report requires a readable correctly identified dump; failed
MiniDumpWriteDump cannot print success. Parent harness verifies actual termination and retains original fault reason.

<a id="diag-5b"></a>
## DIAG.5b — Linux crash and hang capture

Use minimal async-signal-safe recording or a qualified external collector. Install and manage alternate signal stacks
for every relevant OS thread; collect original registers, signal and process/thread identity without allocating,
formatting or taking general locks in the handler. Handle installation failure, recursive faults, signal chaining,
core-policy permissions and normal uninstall. Symbolization/backtraces occur outside compromised execution.

Acceptance: faults on worker/fiber stacks, exhausted stacks, logger/allocator lock ownership, secondary signals,
unwritable paths and missing core collector. SIGKILL/OOM-killer cases retain previous records or explicitly report
no final capture; do not promise a signal handler for uncatchable termination. No global sysctl/security changes.

<a id="diag-5c"></a>
## DIAG.5c — assertion and logging failure policy

Separate recoverable developer assertions from fatal contract violations. Headless/CI defaults cannot show modal
dialogs or silently ignore required validation. Bound recursion and flushing; route catastrophic failure directly
to the emergency channel. Preserve ordinary async logging but define queue pressure, severity retention, source/name
lifetime, sink failure, deregistration and shutdown. Do not label the existing allocating ring sink crash-safe.

Acceptance: assert from logger sink, OOM during formatting, stuck flush, ignored-site churn, missing console and
assert before initialization/after shutdown terminate or return the declared outcome promptly with evidence.
Interactive break behaviour stays explicit; shipping malformed-input rejection cannot rely on compiled-out assertions.

<a id="diag-5d"></a>
## DIAG.5d — symbols, bundles and trustworthy import

Implement the bundle schema and symbol lookup by actual binary/module identity: PDB GUID/age where applicable,
ELF build ID and matching debug files, later dSYM/WASM identities. Retain unloaded module generations and source
mapping. Package artifacts atomically with partial-section recovery, quota/retention and restricted raw-data handling.
CLI import/inspect is bounded and does not execute content.

Acceptance: symbolize an optimized fresh-machine crash, reject wrong symbols instead of giving plausible lines,
recover truncated bundles, reject traversal/oversized/decompression-bomb inputs, retain old DLL symbols after reload.
Golden manifests preserve schema migration and explicit absent sections. Raw dumps are never declared fully redacted.

<a id="diag-6a"></a>
## DIAG.6a — profiler self-correctness and application lifecycle

Repair and qualify allocator/counter/name/thread registries, sampling snapshots and shutdown. Read the actual sample
ring publication protocol; head/tail atomics alone do not prove overwritten payload is safe for concurrent readers.
Use immutable generations or a documented synchronization scheme. Separate registered live count from high-water
index, and preserve frame/history labels after allocator slot reuse. Fix DG08 if its test confirms the concern.

Acceptance: snapshot/save while producers wrap buffers and register/unregister; simultaneous capture/shutdown;
dynamic names from unloaded modules; allocator destruction while sampling. No torn records, stale dereferences or
mislabelled history. Preserve explicit dropped/corrupt/unavailable counts and compatibility with existing captures.

<a id="diag-6b"></a>
## DIAG.6b — CPU, job, GPU and I/O attribution

Connect real frame-graph timestamps to crd-perf and model CPU scopes, logical tasks, queue wait, execution, GPU
submissions and transfers separately. Support multiple queues/devices and disjoint/unsupported clocks. Add calibrated
clock samples/uncertainty where supported, otherwise retain separate tracks. Do not introduce submit-and-wait stalls
merely to make profiling convenient. Correct retired rhi examples and replace disconnected diagnostic assembly.

Acceptance: an existing authored render/compute workload has linked CPU/task/pass/resource identities on DX12 and
Vulkan, including delayed query resolve, ring reuse, timestamps unavailable, wrap and device loss. Compare total
timing to an independent capture; do not subtract unrelated CPU/GPU timestamps or infer exact GPU failure location.

<a id="diag-6c"></a>
## DIAG.6c — capture interoperability and sampling runbooks

Version CPROF without silently changing pinned layouts; add a bounded Perfetto-compatible export preserving task
flows, counters, clock domains, loss and source identity. Keep the native viewer optional. Provide verified WPR/perf
sampling and platform debugger recipes to inspect uninstrumented work, blocked time and memory pressure. External
tool dependencies stay optional; exported data must remain usable offline without a service account.
Where supported, include context switches, page faults, cache/branch counters, lock contention and CPU/NUMA affinity;
record permissions, multiplexing and missing PMU events rather than treating unavailable counters as zero. Profile
frame pacing/input-to-present separately from offline throughput and shader execution time.

Acceptance: export/import a real capture, inspect the same critical path in native and external views, and reject
schema/endianness/size mismatches. Qualify optimized/fiber stack unwinding or explicitly mark missing stacks. A
known unscoped CPU hotspot is visible through sampling; scope-only charts do not satisfy this case.

<a id="diag-7a"></a>
## DIAG.7a — common GPU validation and object identity

Extend existing provider validation capture through startup, recording, submit, asynchronous completion, present,
reload and destruction. Attach stable Cerid resource/program/pass IDs and generation to native object names and
reports; preserve live/recently retired descriptor/range provenance. Define actual activation and unsupported reasons
for core, synchronization and GPU-assisted modes without importing vendor types into public modules.

Acceptance: intentional lifetime/state/descriptor hazard on a small isolated workload produces a correlated error
on each claimed route; ordinary valid consumers stay clean. Dropped validation messages are not a clean run. RAH-6.b
later extends this same service to its new recording contracts; it cannot create a parallel validation collector.

<a id="diag-7b"></a>
## DIAG.7b — DX12 validation and device removal

Qualify debug layer, optional GPU-based validation and DRED setup before device creation. Capture supported
breadcrumbs/page-fault allocations, actual HRESULT/removal reason, recent resource retirement and program mapping.
Preserve last-known diagnostic state through failed query calls and provider shutdown. Report available capabilities
rather than assuming every adapter can page-fault-report.

Acceptance: controlled device-removal/fault handling and safe isolated invalid workloads produce readable bundles;
separate simulated error-path coverage from an actual adapter/device-loss reproduction. Never intentionally hang the
desktop GPU outside a contained qualified test. Software-provider evidence is labelled as such; real hardware gates stay visible.

<a id="diag-7c"></a>
## DIAG.7c — Vulkan synchronization and device faults

Qualify validation layers, synchronization checks, optional GPU-assisted checks and supported device-fault extension
collection. Pin SDK/layer versions; account for shader instrumentation resource limits. Preserve queue-family, access,
layout, descriptor and program identity. Query capabilities before enabling optional modes and retain original
VK_ERROR_DEVICE_LOST information when further calls fail.

Acceptance: controlled missing-barrier/stale-resource cases, asynchronous reports after fence completion, layer absent,
descriptor-limit fallback and device-loss capture on declared Windows/Linux tuples. External RenderDoc/vendor tools
may supplement evidence, never replace the public reporting path or constitute unsupported platform qualification.

<a id="diag-8a"></a>
## DIAG.8a — authored source and lowering provenance

Connect existing CEIR source locations/stable IDs and CKIR node identity to command, executable plan and provider
diagnostics. Preserve many-to-many origins through optimization/fusion and binary cooking; unknown attribution must
say unknown. Map native intrinsic/script entry points and asset generation, retaining debug metadata after reload.
No new evaluator or alternate shader authoring language. Keep semantic program identity independent of viewer layout.

Acceptance: intentionally invalid text/node input, runtime host error and GPU validation error navigate to the
responsible source/node or explicit closest-known origin. Repeat after serialization, optimized lowering and failed
reload. Future CHIR source syntax and actual JIT integration extend this same schema under LANG, not a false current claim.

<a id="diag-8b"></a>
## DIAG.8b — runtime inspect, stepping and safe stop

Expose safe-point inspection/breakpoints via existing executor hooks and production compiled execution seams.
Distinguish task pause, whole-host pause and nonpausable GPU/real-time work; preserve cancellation and timeout.
Snapshot values with types/units and redaction using retained owners, not arbitrary dereference from UI/agent requests.
Define native debugger coexistence, optimized-away values and hot-reload breakpoint rebinding.

Acceptance: inspect/step an authored program in headless and sandbox consumers, pause with pending jobs, resume/cancel,
reject stale-generation requests, and report unavailable values. No worker may block the only thread needed to answer
the diagnostic command. Existing language/runtime capabilities bound what is implemented now.

<a id="diag-8c"></a>
## DIAG.8c — typed human and agent diagnostics commands

Extend existing command/reflection surfaces for capabilities, capture start/stop, bundle inspection, jobs/waits,
allocation/resource summaries, program provenance and replay preparation. GUI/CLI/RPC consumers use the same typed
services. Define schema version, read/record/fault-inject privileges, output bounds, cancellation and deterministic
pagination. Remote enablement/upload and process-memory access require distinct authority; assets cannot grant it.

Acceptance: a native caller and existing CLI/agent transport obtain the same bounded result; unauthorized/stale/
oversized requests fail before expensive work. Prove process-local operation without a network/MCP dependency.
Future CR-D007/MCP transports bind these services and preserve their authorization checks.

<a id="diag-9a"></a>
## DIAG.9a — reproducible inputs and determinism envelopes

Record build/configuration, assets/cooked hashes, random streams, initial state, clock/time-step inputs, input events,
external I/O completions and relevant schedule choices at production boundaries. Define event replay, task schedule
replay and backend-specific numerical replay as different guarantees. Store immutable artifacts or mark missing inputs;
do not assume the current checkout reproduces an older captured generation.

Acceptance: reproduce a seeded authored-runtime failure after process restart and asset edits, detect first divergent
state/event, and reject incompatible replay explicitly. GPU nondeterminism uses a declared tolerance/oracle; no claim
of bit identity across all hardware. Network/physical effects are stubbed only in explicit test replay, never silently rerun.

<a id="diag-9b"></a>
## DIAG.9b — triggerable flight recording and fault injection

Add bounded pre/post-trigger recording for assertions, allocation failures, frame hitches, queue starvation and
resource pressure; retain loss metadata. Support test-only seeded allocator failure, delayed/cancelled completion,
partial I/O, device-loss simulation and reload interruption through existing seams. Capture the trigger and causal
inputs even when the faulty operation cannot complete.

Acceptance: each trigger yields evidence from before the event, bounded post-window or crash truncation, and a
replay/minimization recipe. Inject during load/cook/install/execute/reload/teardown; previous working assets survive
recoverable failure. Shipping capability checks reject injection and arbitrary external I/O replay.

<a id="diag-9c"></a>
## DIAG.9c — rare-failure escalation and minimization

Provide verified rr/TTD recipes on eligible hosts, including tool/version/permission checks, symbols, original exit
codes, storage bounds and offline replay. Add seed/choice/input minimization around the Cerid reproduction harness.
Preserve natural uninstrumented failures alongside instrumented traces to detect observer effects. Hardware watchpoints
and external sampling are documented bounded options; no machine-wide security downgrade or compulsory service.

Acceptance: a small intentional corruption is found by replay/backward inspection on each claimed tool route; a
missing tool or incomplete recording is instrument failure. A real-multicore test remains separate from rr's serialized
execution. Platforms without reverse-debugger support retain event/schedule evidence and an explicit capability limit.

<a id="diag-10a"></a>
## DIAG.10a — stateful fuzzing and evidence-reader security

Extend the existing bounded fuzz framework to allocator operation sequences, capture/bundle/policy readers, current
command decoding, resource reload and compiled/reference CEIR comparisons. Use real production entry points with
memory/time/recursion limits and reference/metamorphic oracles. Register every new ingress surface with a fuzz owner;
future media/network/plugin formats keep corresponding product rows rather than pretending they exist now.

Acceptance: seed malformed lengths, overflow, cycles, truncated writes, invalid generations and destructive import
attempts. Minimize/adopt reproducible findings and replay corpora in relevant CI. Reinvestigate retained unknown-origin
artifacts under the strengthened harness without converting a clean replay into an invented explanation.

<a id="diag-10b"></a>
## DIAG.10b — coverage and diagnostic effectiveness

Measure branch/region/error-path and state-transition coverage for the DIAG scope with the instrumented denominator
explicit. Include mutant/seeded faults that must be detected; do not chase a percentage while missing key error paths.
Track coverage exclusions and flaky/nonreproduced cases as owned evidence. A quarantined test cannot close a gate.

Acceptance: broken sanitizer activation, suppressed reports, removed guard, lost fatal record and incorrect symbol
mapping make the diagnostic suite fail. Read JSON/JUnit and actual artifact contents; exit status alone is insufficient.
No implementation-mirroring unit tests substitute for these observable failure cases.

<a id="diag-10c"></a>
## DIAG.10c — correctness and pressure sanity probes

Add reusable optional probes for NaN/Inf, shape/stride/range, units/time-domain mismatch, unexpected numerical mode,
resource leaks, queue/VRAM pressure and exhausted budgets at existing execution boundaries. Define precision/FTZ/DAZ/
rounding expectations without changing mathematical algorithms or physical units. Run warm/cold, long-soak, repeated
reload and shutdown workloads with one changed stressor at a time and seeded failure injection.
Include file/OS-handle, mapped-memory, device-object and callback registrations in lifetime accounting, not just heap
bytes. Record accessible driver/device-reset and host-pressure evidence; do not label a hardware/OS failure an engine
bug without a discriminating reproduction.

Acceptance: a wrong-but-noncrashing result is caught by an independent invariant/oracle and attributed to a program
generation. Valid special values allowed by a domain are not rejected indiscriminately. Longer offline work remains
progressing; slowdowns are measured before classified. Future scientific/physics/audio modules inherit the probes.

<a id="diag-11a"></a>
## DIAG.11a — reusable host integration and shipping profiles

Integrate lifecycle-managed diagnostics into existing app/headless/sandbox hosts through optional public composition.
Exercise a thin external application consuming installed modules; avoid sandbox-only setup. Default policy is local,
bounded and does not expose a remote debugger. Startup chooses supported modes before device/tool initialization;
dynamic policy changes say when restart is required. Use normal authorable configuration, not hidden C++ algorithms.

Acceptance: a small application runs, deliberately fails, emits an attributable bundle and is investigated without
editing engine source. Add examples for allocator selection, jobs, reload and capture. Verify profiling disabled,
optimized profiling and diagnostic binaries; package symbols/policies intentionally and test clean-machine consumption.

<a id="diag-11b"></a>
## DIAG.11b — cost, CI and fast investigation workflow

Measure the declared budgets on allocation-heavy, tiny-job, mixed I/O, authored rendering and offline workloads;
report p50/p95/p99, memory, drops, I/O and capture completion. Compare off/basic/full modes and independent external
sampling; use peer tools only on comparable features. Store measured boards at measurement time, including losses.
Extend dev.py/CI ownership and conclusion artifacts to diagnostic specimens and mode activation.

Acceptance: focused local plan selects affected tests and the relevant risk route; hosted Windows/Linux sanitizer,
race and short corpus/negative-control jobs have nonzero execution and usable artifacts. Longer fuzz/soak runs use
nightly/manual cadence. No blanket local sweep. Document symptom→command→artifact→next discriminating test and human-only publication.

<a id="diag-11c"></a>
## DIAG.11c — native qualification and portable capability boundary

Qualify actual Windows/Linux CPU/GPU/driver/build tuples and unsupported-mode handling, including symbols, optimized
execution and current providers. Validate portable schemas and host adapters contain no x64/native-filesystem-only
assumption in public contracts. Add later MAC.DIAG/WEB.DIAG contract hooks without claiming their execution passed.
Native ARM64/fiber evidence belongs to JOBS-PORT and its consumers, and cannot be inferred from x64 success.

Acceptance: one complete native diagnostic workflow per required available tuple, plus missing tools/hardware documented
as open qualification, not pass. A software Vulkan adapter qualifies that adapter only. Do not procure/register new
hardware/services. DIAG cannot close on required Windows/Linux evidence that is unavailable; later-platform rows stay explicit.

<a id="diag-12"></a>
## DIAG.12 — independent end-to-end close audit

Re-read every child and DG finding against final source, public consumers, artifacts and CI. Use the same app to
demonstrate allocator UAF, job race, wait failure, fatal crash, authored-program error and GPU hazard in isolated
specimens, then demonstrate repaired versions. Confirm capture import/inspection, source attribution and privacy.
Remove obsolete diagnostic scaffolding/examples and duplicated services; retain useful external tooling recipes.

Acceptance: all claimed detection classes pass both faulty and valid controls; all required Windows/Linux evidence
is tied to the published code; documentation/recipes/bench/indexes are current; every residual capability limitation
has an explicit owner. DIAG closes only after every child, never because code builds or a dashboard looks complete.

<a id="later-platforms"></a>
## MAC.DIAG and WEB.DIAG — later native and browser release gates

MAC.DIAG uses the same schemas/policies/profiler and qualifies Apple Silicon/native scheduling, dSYM identity,
sanitizers available on that toolchain, Instruments interoperability, Metal validation/command-buffer failures,
permissions and crash collection. Test actual OS/hardware; a Metal emitter or CI compile is insufficient.

WEB.DIAG qualifies WASM source maps/debug identities, Emscripten memory instruments, worker/nonthreaded modes,
memory growth, browser event-loop progress, asynchronous WebGPU errors/device loss, timestamp availability/precision,
tab suspend/termination, bounded persistence/export and unavailable native tools. Feature detection and restricted
download/export replace raw process/filesystem assumptions. Test actual supported browsers, secure context/isolation
deployment and storage quotas. No universal native dump or GPU instruction replay promise.

## Existing programme ownership after DIAG

CORE-USE revalidates renderer-specific allocation/task/retirement consumers using DIAG; DG05's immediate repair is
DIAG.1a. MEM-HARD/JOBS-HARD retain broader product/peer/performance/new-platform qualification; they reuse early
diagnostic mechanisms and add evidence as consumers grow. SANITY-TLSF retains later newly introduced consumer audits.
RAH-6.b owns new frame recording/access contracts using DIAG's validation surface. D7E-7/D7E-10/I2D-9.e own the polished
crd-ui views of existing diagnostics. LANG owns full CHIR/native scripting and future compiler/debugger integration.
OPS.1/OPS.2 extend DIAG to future distributed/product deployments and migration matrices. No existing requirement is
deleted, and no duplicate live bug owner remains for the early mechanism. Record new gaps in ROADMAP under the earliest
responsible child; this document must not accumulate an independent Next list.
