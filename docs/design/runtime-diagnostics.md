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

Settled (2026-10-24, DIAG.12, [session](../sessions/2026-10-24-diag-12-one-recorder.md)): the DIAG.2b
`DiagnosticRecorder` had no production consumer; it is removed, and each clause names its carrier. The preallocated
ring, a full ring, concurrent recording, early-init and late-shutdown no-ops and a fatal path that finds the ring held
are `FlightRecorder`'s (DIAG.9b, its `flight recorder:` and `flight trigger:` cases). The emergency record is the crash
capture's record (DIAG.5a/5b), written without a lock or an allocation. Registration lifetimes are their owners':
profiler names and allocator entries (DG08, DIAG.6a), the jobs observer (DIAG.10c) and unloaded modules (DIAG.9b).

<a id="diag-3a"></a>
## DIAG.3a — complete allocator contracts

Inventory malloc, linear/growable-linear, stack, pool/growable-pool, TLSF/growable-TLSF, virtual-memory, ring, offset,
streaming/category and thread-safe wrappers, plus backing/chunk allocators discovered in the census. Declare owner,
alignment, zero-size, overflow, exhaustion, reset/rewind, reallocation and concurrency semantics for each. Offset/GPU
allocators validate logical ranges/lifetimes; do not apply CPU ASan poisoning to an unmapped device address.

Acceptance: checked size/stride/add/multiply/alignment/page arithmetic, failure preserving previous allocation,
wrong-allocator/free ownership, null and near-address-width boundaries. Cover parent/child arena destruction, external
buffers and partial construction. Preserve production layout/performance where diagnostic metadata can be side storage.

Settled policies (2026-10-06, [session](../sessions/2026-10-06-diag-3a-allocator-boundaries.md)):
- **Wrong-allocator free.** Pool, growable pool, TLSF, growable TLSF and virtual memory, and the wrappers that route to
  them, refuse in every build a pointer they did not hand out, after an assert where asserts are compiled in. Arenas
  (linear, stack, growable linear) ignore every free. The diagnostic decorator reports `UnknownPointer`.
  `MallocAllocator` cannot tell, because `owns()` is true for every pointer; wrap it in the decorator to diagnose.
  (Since 2026-10-19 its block header makes a free whose header fails the check a named fatal, but reading below a
  pointer it never handed out may itself fault, so that is a corruption check, not an ownership test.)
  TLSF also refuses a misaligned pointer and an immediate double free. Interior pointers that land on a valid header,
  and stale frees after reuse, need the decorator or DIAG.3e generations.
- **Partial construction.** An owning arena whose parent refuses its backing memory is out of memory: `CRD_FATAL`
  naming the arena, in every build. A growable pool whose parent refuses a page returns nullptr from `try_allocate`
  and stays usable; its `allocate` is fatal. `construct`/`construct_array` return nullptr for a refused allocation or
  an overflowing count, and undo a throwing constructor (built objects destroyed in reverse, storage returned).
  A consumer that cannot continue without the memory turns a refusal into its own named out-of-memory fatal through
  `allocate_or_fatal`, `allocate_array_or_fatal`, `construct_or_fatal` and `construct_array_or_fatal` (a zero count
  stays the empty array): the containers' growth and fixed buffers (a refused `Array` growth was relocated into null
  and kept its capacity), the CEIR context's IR arena and the sparse-set storage's pools, since 2026-10-17
  ([session](../sessions/2026-10-17-diag-11b-link-time-null-dereference.md)); the scene's sparse-set dense arrays,
  shared component pools and their entries, relation reverse indexes, and the resource manager's control blocks
  (a failed load's block included) and async and streamed load contexts, which followed a refused allocation through
  null in a build without asserts, and the archetype graph and chunk allocator, whose pools' generic fatal now names
  them ([session](../sessions/2026-10-17-diag-11b-unchecked-allocations.md)), and the scene's instantiation staging
  block for a component past 1024 bytes or 16-byte alignment, which before was a 1024-byte stack buffer at 16-byte
  alignment ([session](../sessions/2026-10-17-diag-11b-scene-pack-trust.md)); each has a child-process mode
  (`crd-containers-fatal-*`, `crd-ceir-fatal-*`, `crd-scene-fatal-*`, `crd-resources-fatal-*`, the alloc-fatal
  specimen). A borrowed linear, stack, pool or TLSF buffer that is null, or a borrowed pool span that overflows, is
  fatal in every build as well, and so is a shared component pool past its index range or an entry refcount past u32
  (its doubling ran through u32 and wrapped to an endless loop past 2^31 entries). Since 2026-10-18
  ([session](../sessions/2026-10-18-diag-11b-engine-allocation-refusals.md)) the rest of the engine does too: the
  CEIR reload set's cook Context, generations and their Contexts, the plan cache's copies, the host provider's pooled
  launch tokens, the CEIR diagnostic engine's arena copies, the CKIR reference interpreter's and reverse AD's scratch,
  every hesap-fft plan table and sub-plan, hesap-dense's owning matrix, vector, permutation and structured storage and
  its GEMM/SYRK packing buffers, the DSP convolver's and spectral estimators' plans, the CWT's per-job plans, the
  vectorized HVP tape (its node capacity now checked in every build), the optimization model's functions, the scene
  renderer's staged frame graph and the scene and öbek cookers' registries. hesap-tensor's sparse scratch keeps its
  `AllocFailed` status and refuses a byte size that overflows. `allocate_array` and `allocate_array_or_fatal` take an
  explicit alignment for SIMD and streaming-store tables. Each has a child-process mode (`crd-ceir-cook-fatal-*`,
  `crd-ceir-host-fatal-*`, `crd-kir-fatal-*`, `crd-hesap-*-fatal-*`, `crd-scene-cooker-fatal-*`,
  `crd-scene-render-fatal-*`, three more `crd-ceir-fatal-*`), and `check-allocation-results.py`
  (`crd-check-allocation-results`) fails a raw allocation result in engine, tools, runtime or sandbox code that is used
  without a check, passed up to no caller, and named as no heap site.
- **Parent/child and external buffers.** A child is destroyed before its parent and returns exactly the blocks it took.
  An arena never frees an external buffer, stays inside it, and returns it unpoisoned.

<a id="diag-3b"></a>
## DIAG.3b — allocator-aware sanitizer boundaries

Extend the existing ASan adapter to each addressable allocation and container live range. Correctly model reserve,
commit, padding, allocate/free, shrink/grow, reset, rewind, decommit and pool reuse; account for shadow granularity.
Keep free-list bookkeeping accessible without leaving the entire freed payload unpoisoned. Separate external backing
ownership and nested allocator annotations; annotations must not accidentally unpoison a parent's protected range.

Acceptance: one-byte under/overrun, freed-slot access, rewind use, unused container capacity and small/over-aligned
objects are intentionally detected where declared. Test large/odd-sized slots, metadata manipulation and non-ASan
builds. Immediate same-address reuse remains a documented raw-pointer limit, addressed by DIAG.3e.

Settled policies (2026-10-07, [session](../sessions/2026-10-07-diag-3b-pool-and-arena-poisoning.md)):
- **Pools.** A free slot is poisoned whole, its free-list header included; the allocator and `validate_structure()`
  read a link just in time. A live slot is the logical allocation (`allocation_size()` reports the slot), so pool
  over- and underruns are declared at the slot boundary into a free neighbour. An overrun into a live neighbour is a
  raw-pointer limit (DIAG.3e). A double free still reaches the structural walker rather than an in-allocator report.
- **Arenas.** Linear, stack and growable linear arenas unpoison exactly each slice; alignment padding, the unhanded
  tail and everything after a reset or rewind stay poisoned. Every allocator returns exactly its own range unpoisoned
  to its parent or caller, never more.
- **TLSF** (and growable TLSF, which destroys each chunk's heap before freeing its pool;
  [session](../sessions/2026-10-07-diag-3b-tlsf-and-ring-poisoning.md)). Every block header and every free payload,
  its free-list links included, is poisoned at rest; the allocator and `validate_structure()` reach a header word
  through accessors that open exactly that word and restore its previous state, and each operation re-derives the
  shadow of the blocks it leaves behind (split, merge, leading alignment remainder, in-place shrink and grow). A live
  block is the logical allocation, so one-byte over- and underruns land in a poisoned header whether the neighbour is
  live or free. A test that forges header corruption opens the word first.
- **Ring.** Unclaimed space, alignment padding, wrap waste and retired epochs are poisoned. A claim opens the
  granules it overlaps after its CAS wins; retirement poisons only granules wholly inside the retired span, before the
  tail publishes it, with retirers serialised in ASan builds only. Claims and retirements never write the same
  shadow byte, so the boundary is per granule: byte-exact for claims whose offset and size are multiples of 8.
- **Containers** ([session](../sessions/2026-10-07-diag-3b-container-live-ranges.md)). `Array` marks its unused
  capacity `[size, capacity)` with `__sanitizer_annotate_contiguous_container` (through
  `asan_annotate_live_range`), so an access past `size()` inside the capacity reports `container-overflow`, not
  `use-after-poison`. Every mutator moves the boundary: a slot opens before it is constructed and closes after it is
  destroyed, a new buffer is marked once the elements are relocated, and the marking is lifted before the buffer goes
  back to its allocator, so an arena slice or pool slot returns exactly as it was handed out. Only whole granules
  inside the buffer are marked (an older runtime refuses an unaligned start, and a granule shared with a neighbour
  cannot be split), so a packed buffer never marks a neighbour; detection is byte-exact when the buffer starts on a
  granule and its byte size is a multiple of 8. Writing through `data()` past `size()` is reported: grow with
  `push_back`, `resize` or `resize_uninitialized` first. `String` (an inline buffer, then a heap buffer with its NUL
  slot) and `FixedArray` (inline storage copied with its owner) are not annotated.
- **Granularity.** Detection is byte-exact for 8-byte aligned slices. A partly addressable granule is reported after
  the next granule's shadow: `use-after-poison` before poisoned padding, `unknown-crash` before a live neighbour.
  The negative control requires `use-after-poison`, so its modes keep a poisoned granule after the faulting byte.

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
Stack overflow capture does not rely on the OS default slack: while crash capture is installed, a TLS callback
reserves the last-chance stack guarantee (64 KiB, `SetThreadStackGuarantee`) for every thread the process creates.
This is process-wide: each new thread spends that much of its stack reserve. Threads that existed before `install()`
are not touched (the installing thread is guarded by `install()`; hosts call `guard_current_thread_stack` for others).
Linux has no equivalent creation hook for raw threads and keeps per-thread alternate-stack registration.

Acceptance: crash while allocating/holding a logger lock, concurrent crashes, handler failure, denied/full output,
missing symbols and stack exhaustion. A successful report requires a readable correctly identified dump; failed
MiniDumpWriteDump cannot print success. Parent harness verifies actual termination and retains original fault reason.

<a id="diag-5b"></a>
## DIAG.5b — Linux crash and hang capture

Use minimal async-signal-safe recording or a qualified external collector. Install and manage alternate signal stacks
for every relevant OS thread; collect original registers, signal and process/thread identity without allocating,
formatting or taking general locks in the handler. Handle installation failure, recursive faults, signal chaining,
core-policy permissions and normal uninstall. Symbolization/backtraces occur outside compromised execution.
An alternate signal stack another runtime already installed on a thread (a sanitizer, a test framework) is
borrowed, never replaced, and left with its owner on uninstall; only a stack crd installed is disabled again.

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

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-7b-h1-warp-fault-and-pass-resolution.md)):
- **No real fault on WARP.** WARP discards stores to an unmapped GPU address (released, far past a live buffer, wild)
  and has no watchdog for a shader that never ends; a forced removal leaves DRED's breadcrumb list empty, as on
  hardware. The real fault is therefore the hardware gate (h2): one contained page fault in a child process, started
  by the user behind `CRD_DX12_HARDWARE_FAULT_OPT_IN=1`, never in CI. The WARP run of the same child stays as
  CI-capable, labelled software evidence of the whole path.
- **In-flight pass.** Each breadcrumb node keeps its op history and context strings (bounded), and names the pass the
  GPU was inside when it stopped: of the `BeginEvent` markers open at the stopping op, the innermost whose context
  carries a Cerid Pass identity. This is the DX12 pass-to-fault route of DIAG.7a; the removal bundle is version 2
  (version 3 since DIAG.9b added the `Simulated` origin).

<a id="diag-7c"></a>
## DIAG.7c — Vulkan synchronization and device faults

Qualify validation layers, synchronization checks, optional GPU-assisted checks and supported device-fault extension
collection. Pin SDK/layer versions; account for shader instrumentation resource limits. Preserve queue-family, access,
layout, descriptor and program identity. Query capabilities before enabling optional modes and retain original
VK_ERROR_DEVICE_LOST information when further calls fail.

Acceptance: controlled missing-barrier/stale-resource cases, asynchronous reports after fence completion, layer absent,
descriptor-limit fallback and device-loss capture on declared Windows/Linux tuples. External RenderDoc/vendor tools
may supplement evidence, never replace the public reporting path or constitute unsupported platform qualification.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-7c-descriptor-limit-and-stale-layout.md)):
- **Descriptor-limit fallback.** GPU-assisted instrumentation binds its own set at the device's last slot
  (`maxBoundDescriptorSets - 1`), and the layer does not lower the limit it reports. The context therefore reports the
  slot (`VulkanValidationLayer::instrumentation_set_slot`, `instruments(n)`). The capture classifies the layer's GPU-AV
  setup warning (`WARNING-GPU-Assisted-Validation`, pinned to the SDK's layer) as an instrumentation refusal, carried
  by the common `ValidationReport::instrumentation_failures`. A run that saw one is not clean, because the refused
  work ran unchecked. The layer's `GPUAV-Compile-time-*` warnings (a shader instrumented with more than 75 buffer
  checks) report the instrumentation's own cost; the capture counts them as `ValidationReport::cost_notices`, not
  as warnings (2026-10-24, [DIAG.11b](../ROADMAP.md#slice-diag.11b)).
- **Asynchronous reports.** GPU-AV results arrive only once completion is observed (the fence wait), never at record
  or submit. On VVL 1.4.341 the Core image-layout mismatch, although it names `vkQueueSubmit`, also arrives at the
  wait. Gates assert "nothing at record, correlated by the wait"; they never assert the submit-time count.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-7c-g-loss-bundle.md)):
- **Loss bundle.** The first time a device is recorded lost, after its `VK_EXT_device_fault` report is read, the
  completion seam writes a `VkLossRecord` (first failure, loss origin and operation, failure counts, adapter, fault
  report; no pointers) as a `DeviceRemoved` live dump (`gpu_*.dmp`) whose evidence stream starts with its own magic
  (`CRVL`) and version. It is written once per device and never for a failure that is not a loss. A simulated loss is
  labelled `Simulated`. The reader refuses foreign, other-version and other-size evidence, including DX12's.
- **A loss after another failure is still a loss.** `VkDeviceFailure::lost()` follows the first
  `VK_ERROR_DEVICE_LOST`, not only a loss that was the first failure: a bounded wait that times out before the device
  is reported lost no longer hides the loss from the fault query, the bundle or the lost-device short cut.
- **Linux.** Live dumps are a Windows facility (DIAG.5a); on Linux the loss is recorded and kept in process and the
  write reports `Unsupported`. A Linux on-disk loss bundle would need a non-fatal Linux evidence writer, which DIAG.5b
  does not provide.

<a id="diag-8a"></a>
## DIAG.8a — authored source and lowering provenance

Connect existing CEIR source locations/stable IDs and CKIR node identity to command, executable plan and provider
diagnostics. Preserve many-to-many origins through optimization/fusion and binary cooking; unknown attribution must
say unknown. Map native intrinsic/script entry points and asset generation, retaining debug metadata after reload.
No new evaluator or alternate shader authoring language. Keep semantic program identity independent of viewer layout.

Acceptance: intentionally invalid text/node input, runtime host error and GPU validation error navigate to the
responsible source/node or explicit closest-known origin. Repeat after serialization, optimized lowering and failed
reload. Future CHIR source syntax and actual JIT integration extend this same schema under LANG, not a false current claim.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8a-ceir-provenance.md)):
- Provenance is a Context side table plus the skippable binary `ORIG` chunk (`crd/ceir/provenance.hpp`), never op
  content: it is outside `stable_hash`, CSE equality and the printed text, so reformatting a source changes no content
  or interface hash. `Operation::loc` stays content (builder-declared) and is the fallback when no origin is recorded.
- An origin is `{SourceLoc, node, space}`; spaces are the carrying op, another CEIR op by stable id, and a CHIR node.
  A pass that replaces ops records the union of their origins (CSE survivor, folds), naming each replaced op by the
  stable id it had before erasure; drivers assign stable ids first. A no-op removal leaves no origin.
- Unknown attribution is a typed gap: NoOperation (nothing to blame) or NoSourceLocation (the op's id is the
  closest-known origin). `ORIG` is written only when some op has an origin, so origin-free blobs are unchanged.
- The compiled plan owns per-instr sites resolved at compile (outliving the Module), read only on diagnostic queries;
  `CompileResult::op` and `RunResult::fault` name the innermost offender; the parser reports `line:col` and the file.
- Cook and reload (2026-10-07, [session](../sessions/2026-10-07-diag-8a-cook-and-reload-provenance.md)): a text
  cook takes the authored file and its blob carries the positions; a failed cook reports a POD `CookSite` (line, column,
  offender id, gap) that outlives the transient cook Context and rides `AddResult`/`ReloadResult`. The installed
  generation keeps its own positions through failed reloads; a `NoChange` reload whose candidate has origins refreshes
  them op-for-op (handles stay current), an origin-free candidate does not, and plans compiled earlier keep theirs.
- CHIR lowering (2026-10-07, [session](../sessions/2026-10-07-diag-8a-chir-lowering-provenance.md)): `lower_chir(m,
  ctx, file)` records an `OriginSpace::ChirNode` origin (CHIR StableId plus line:col) on every op it creates. `file` is
  registered in the CEIR Context and stamped on every positioned node; the model's own file numbering is never copied
  (no file = id 0, line:col kept). A `core.state` cell names its StateDecl and the folded StateUpdate. A Query creates no
  op. A graph-authored node has no position and keeps its CHIR id; `render_provenance` lists such position-less
  origins after the gap, so the closest-known authored node is always named.
- Host provider (2026-10-07, [session](../sessions/2026-10-07-diag-8a-host-provider-provenance.md)): an error raised
  inside a body that runs on a separate sub-interpreter (a `task.parallel_for` or `map_reduce` map index, a `map_reduce`
  fold step, a pooled `async.launch`/`task.*` launch body) is blamed on the body op that raised it
  (`Interpreter::failed_op()`), in both the crd-jobs provider and the sequential reference, matching the plan's
  innermost `RunResult::fault`. The owning op is blamed only when the body names no op. Parallel indices keep
  first-in-index-order, so the reported op does not depend on the job split. A pooled body's error is reported at its
  await, naming the body op.
- Entries and generations (2026-10-07, [session](../sessions/2026-10-07-diag-8a-entries-generations-and-chir-reload.md)):
  an entry refusal has no op to blame, so a lookup-failed `NoEntry` (no symbol table, or no such symbol) carries an
  owned copy of the requested name (`ExecResult::entry`, `CompileResult::entry`); a found entry with the wrong arity
  is blamed on the entry function's authored line. A fault in a hot-reloaded program is located in the generation that
  ran it, never in whatever is installed now: `ReloadSet::locate(handle, ...)` matches the handle's generation number
  (Current, Retiring = the held zombie, Gone = no longer held) and renders the site in that generation's own Context
  with its content hash; a Gone generation is named by asset and number only. A CHIR-lowered program cooked with
  `cook_program` keeps its `ChirNode` origins through failed reloads, and a NoChange reload refreshes them with the
  same space and CHIR ids.
- Intrinsic sites (2026-10-07, [session](../sessions/2026-10-07-diag-8a-intrinsic-provider-sites.md)): an error at an
  ADR-0110 intrinsic names its native provider through `native_binding(ctx, op)`, which reads the registration's
  `[op.native]` binding; a kind the Context does not register is `Unregistered` (unknown there), never non-intrinsic.
  `render_op_site` is the one rendering every reporter uses (`<op> native <provider> at <provenance>`), so no result
  struct carries a provider copy. A host-evaluated intrinsic's refusal is blamed on its own op: the scene resolver
  sets `SceneResolvedHandles::fault` for an unwired or 0-returning callback and for the op `find_scene_misuse` names.
- GPU dispatch and CKIR record sites (2026-10-07, [session](../sessions/2026-10-07-diag-8a-gpu-dispatch-and-ckir-sites.md)):
  with a `DispatchSites` table, `execute_lowered` mints one `ObjectKind::Pass` identity per recorded dispatch and
  wraps it in the recorder's debug label `[<id>] <op name> @<kernel>` (`ComputeRecorder::begin_label`/`end_label`,
  default no label; Vulkan records a debug-utils label). The Vulkan capture keeps the innermost label identity in
  `ValidationMessage::label` even when a named object supplies `identity`, so a validation error at a dispatch maps
  through `DispatchSites::find` to the dispatch op and its authored origin; the `@<kernel>` symbol is the link to the
  CKIR kernel. The table retires its identities; a dispatch the backend could not label is recorded `labelled = false`
  (an explicit gap), and a refused `execute_lowered` names the refused op (`fault()`). A refused `.ckir` names its
  positional record (`CkirReadResult::pool`/`index`; CKIR refs are positional, so the index is the node identity) and
  its line and column: the offending token, or the record's `[[...]]` header for a post-parse bounds check.
- CHIR graph documents (2026-10-07, [session](../sessions/2026-10-07-diag-8a-chir-graph-schema-sites.md)):
  `read_schema` returns `SchemaReadResult` in the `ChirParseResult` mold. A refusal names the offending record's
  1-based line and token column and the CHIR node the record declares or names, with its stable id, which both
  projections derive identically, so it also locates the node in the CHIR text. A layout row that resolves to no
  node names its unresolved id (`orphan`). The document is line-oriented: fields sit on their record's line, a
  missing field is reported where it was expected, and a trailing token is refused. A second writer into an in-pin
  is blamed on the later edge (the consumer node), and the whole-graph checks report in document order through
  read-order side lists, because the model stores edges canonically and layout last-write-wins.

<a id="diag-8b"></a>
## DIAG.8b — runtime inspect, stepping and safe stop

Expose safe-point inspection/breakpoints via existing executor hooks and production compiled execution seams.
Distinguish task pause, whole-host pause and nonpausable GPU/real-time work; preserve cancellation and timeout.
Snapshot values with types/units and redaction using retained owners, not arbitrary dereference from UI/agent requests.
Define native debugger coexistence, optimized-away values and hot-reload breakpoint rebinding.

Acceptance: inspect/step an authored program in headless and sandbox consumers, pause with pending jobs, resume/cancel,
reject stale-generation requests, and report unavailable values. No worker may block the only thread needed to answer
the diagnostic command. Existing language/runtime capabilities bound what is implemented now.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8b-safe-point-inspection.md)):
- The two executors offer safe points and nothing else: the compiled plan through `plan::RunControl` (a null-default
  parameter of `plan::run`, separate from the observation-only `RunHooks`; a safe point before every instr) and the
  reference interpreter through its §112 `pre` step hook. `crd/ceir/inspect.hpp`'s `Session` decides whether the
  executing thread stops there. A pause blocks the executing thread at the safe point (mutex and condition variable;
  no signal, trap instruction or thread-context change, so a native debugger can attach and break independently).
- Every inspection is a request the paused executing thread answers from its own state (`plan::read_value`,
  `Interpreter::value_of`); a controller never dereferences executor memory and every controller wait has a timeout.
  A safe point on the declared controller thread is refused (`SameThread`) rather than blocking the only thread that
  can answer.
- A value reads only from the safe point's own call frame and is `Available`, `NotYetComputed`, `OutOfScope`,
  `OptimizedAway` (no instr defines it; the survivor that carries it as a `CeirOp` origin is named), `NoSuchValue` or
  `Redacted` (the host's `RedactFn`, the value's retained owner, withholds the bits but not the type). A snapshot carries
  the IR type, its canonical text and a quantity's dimension; the plan keeps each result's type for this.
- Cancel and the fuel budget are preserved: a cancel at a pause ends the run with `Cancelled` blamed on the held instr
  or op before it runs (`RunError::Cancelled` is new); pausing consumes no fuel.
- A pause request lands at the safe point the executing thread reaches next, before whichever instr runs next in any
  frame, a call's callee or a loop's body included. The held instr has not run: its op reads `NotYetComputed`, or
  `NoSuchValue` when it defines no result (`core.for`). A case pins landings on a resultless instr, a loop body and a
  callee by holding the executing thread at a chosen safe point through the session's observer (2026-10-17,
  [session](../sessions/2026-10-17-diag-11b-zstd-configure-and-pause-landing.md); a free-running request had landed on
  `core.for` once under TSan against an oracle that expected `NotYetComputed` everywhere).
- Scopes are typed: `Task` pauses one execution, `WholeHost` runs the host's freeze/thaw around the pause (refused
  without them) and `NonPausable` never waits (hits are counted).
- A session is bound to one generation (the host's number, e.g. a ReloadSet handle's) and refuses requests naming
  another before any work. Breakpoints are authored `file:line` positions resolved through DIAG.8a provenance on every
  bind, never stable ids (pre-order ids shift when an op is inserted) or compiled positions, so a hot reload rebinds
  each line to what the new generation authored there.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8b-pending-jobs-and-detached-bodies.md)):
- A host executor attaches through `inspect::HostLink` (`HostProvider::execute(..., session)` for crd-jobs). Its
  cooperative cancel flag becomes the execution's only flag: `Session::cancel` raises it, so work on pool workers
  stops too, and a pause re-reads it every 2 ms, so the host's own cancel ends a paused execution.
- Work the host started keeps running through a `Task` pause, and each stop reports how much of it is not yet joined
  (`StopRecord::pending_jobs`; for crd-jobs, pooled launches not yet awaited). The submitting thread cannot stop inside
  a parallel range or a fold (it is waiting on the pool there), so a pause request lands at its next safe point.
- Bodies a host runs on its own sub-interpreters (pooled launch bodies, parallel ranges, map_reduce fold steps) are
  detached: they never pause, because a held pool worker could be the one the submitting thread waits on and a
  session holds one stop at a time. A bound breakpoint there is counted and refused `DetachedBody`, never missed
  silently; a launch the provider runs in-frame (it captures an outer value) pauses like any other op.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8b-gpu-dispatch-non-pausable.md)):
- GPU work is classified non-pausable at the seam that records it, not by the session's declared scope: a recorded
  dispatch runs later on the device, where no safe point exists. crd-ceir-gpu's `execute_lowered` takes an optional
  `DeviceInspect` (session, generation, refusal out); it attaches the recording through `Session::begin_device`
  before any work (`NotBound`, `StaleGeneration` or `Busy` returns `InspectRefused` with nothing recorded) and calls
  `Session::device_point` before recording each dispatch. A breakpoint bound to the dispatch op (module-form bind) is
  counted (`device_hits`) and refused `NonPausable`; it never waits, even under a `Task` session.
- While a recording is attached, `request_pause` is refused `NonPausable`; a cancel stops the recording before the
  next dispatch (`ExecuteError::Cancelled`, blamed on that dispatch's op through `DispatchSites`); work already
  recorded is the caller's to submit or discard.
- A recording is exclusive: it is refused `Busy` while another execution or recording is attached, on any thread. A
  host op that records device work in the middle of an interpreter execution is therefore not supported yet. Only
  `execute_lowered` is classified; `execute_rt_lowered`, `execute_work_lowered` and the render executor take no
  session.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8b-inspect-host-and-headless-consumer.md)):
- Consumers compose one host, crd-ceir-cook's `InspectHost`: it cooks the authored text under its file name into a
  ReloadSet generation, compiles one entry, binds the session to the generation the set minted and runs the plan on
  an executing thread it owns. The consumer's thread is the controller (declared at construction) and talks to the
  execution only through the session's bounded requests, so neither waits on the other. Every install and every start
  rebinds, so a request naming a replaced generation is refused `StaleGeneration`; a load or start while an
  execution is attached is refused `Busy`; a rejected or failed reload keeps the last good generation; the destructor
  cancels and joins a paused execution. The session and the executing thread allocate from the host's own allocators.
- A consumer names a value by its authored line (`inspect::op_at_line`, the breakpoint resolution), never by a stable
  id it guessed. The authored demo program is a committed asset (`assets/ceir/inspect_demo.ceir`, bootstrapped by
  print and kept by an anti-drift check against its builder).
- The headless consumer is `ceridc inspect`: it validates the whole request first, then reports every stop (authored
  line, column, depth, op), each watched line's typed value and the scripted action, bounds the stops it reports
  (past the bound it cancels and says `truncated`) and every wait, and exits nonzero unless the run finished or the
  script cancelled it. It is CLI-only: the agent transports gain typed inspection authority in DIAG.8c.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8b-sandbox-consumer.md)):
- The sandbox consumer is `crd-sandbox --inspect [id]` through a sandbox-internal panel (`sandbox/src/inspect_panel`)
  over the same `InspectHost`. The program is an asset loaded app-first through the renderer's program seam
  (`SceneRenderer::resolve_program_text`: an `app://<id>` file shadows the shipped `engine://<id>`; `ceir` is a
  registered program folder, `ceir/<name>.ceir`), never a bare file read, so an application replaces it without an
  engine edit. Its asset id and breakpoint file name are the canonical folder/name, not the winning mount, so a
  reload that changes which mount wins keeps the asset and rebinds every `file:line`.
- The frame loop is the controller and never waits on the program: each frame polls for a stop with a zero timeout,
  takes the watched values once per new stop (the paused executing thread answers each bounded snapshot) and draws
  from that cache, so frames keep presenting while the program runs or is held. A stop is new only when its sequence
  differs from the one already taken: a cancel accepted at a stop leaves the session on it until the executing thread
  leaves, and must not be read as another stop. Commands carry the generation the panel showed; a load while the
  program runs or is held is `Busy`. Scope stays `Task`.

<a id="diag-8c"></a>
## DIAG.8c — typed human and agent diagnostics commands

Extend existing command/reflection surfaces for capabilities, capture start/stop, bundle inspection, jobs/waits,
allocation/resource summaries, program provenance and replay preparation. GUI/CLI/RPC consumers use the same typed
services. Define schema version, read/record/fault-inject privileges, output bounds, cancellation and deterministic
pagination. Remote enablement/upload and process-memory access require distinct authority; assets cannot grant it.

Acceptance: a native caller and existing CLI/agent transport obtain the same bounded result; unauthorized/stale/
oversized requests fail before expensive work. Prove process-local operation without a network/MCP dependency.
Future CR-D007/MCP transports bind these services and preserve their authorization checks.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8c-command-service-and-transports.md)):
- One service, crd-perf's `DiagCommandService` (`crd/perf/diag_commands.hpp`), is what every consumer calls; a
  transport only moves a request in and the response document out. It needs no network, MCP or transport code.
- Authority is a set of distinct classes (`read`, `record`, `inject`, `remote-enable`, `upload`, `process-memory`;
  none implies another) granted by the host when it builds the service. It never travels in a request, so neither a
  request nor an authored asset can raise it. Each command declares one class (DIAG.9a adds an optional second class,
  `also`, that must be granted as well).
- Refusals come before any work and in a fixed order: schema version, unknown command, authority, request bounds
  (`oversized`), arguments (`bad-argument`; `unavailable` when the host granted no file root), stale cursor,
  cancellation. A refusal leaves the retained snapshot alone. Input files are bounded on their size before a byte is
  read; path arguments are relative plain names under the host's root.
- Pagination is deterministic: a cursor-0 request takes one snapshot under a new generation; later pages are cut from
  it by `generation * 2^20 + offset`, so the same cursor and bounds give the same bytes, and any new snapshot makes
  older cursors `stale-cursor`. Pages are bounded by items and serialized bytes; items and string values are clipped
  and say so. Evidence that cannot exist in this process or build answers `unavailable` with the reason.
- Upper modules register their own commands (`register_command`) and get the same checks, so foundation never
  includes their headers. `ceridc diag` and the MCP `diag` tool bind a service built from the process's start-up
  flags (default `read`); the tool's arguments are the request fields only.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8c-program-provenance-command.md)):
- `program.provenance` (`read`, takes a path) is registered by crd-ceir-cook, not crd-ceir: the core's link edges stay
  the host-only substrate (I5), and the cook bridge already loads every program form. A host owns its configuration
  (dialect registrar, byte limit) and registers it into any service; ceridc binds it into both its CLI and MCP
  services through one function, so the transports list and answer the same commands.
- The file's size is bounded before it is read; the form is chosen by its leading bytes (cooked CRDR, CEIR binary,
  else text parsed under the request's relative path, so the host root never appears); each request loads into a
  fresh Context with the host's dialects registered first, so an unregistered op reports `unregistered`, never
  non-intrinsic. A load failure is refused `failed` naming the text's file:line:col, the binary's byte offset or the
  cooked read error.
- One `op` item per op in pre-order (stable id, name, depth, closest-known position, gap, origin count, first CHIR
  origin, native binding, then the rendered provenance last, so clipping never removes a position). When the op
  item cannot state every origin (more than one, or one naming another op), one `origin` item per origin follows it:
  a many-to-many origin is never lost to a clipped string.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8c-gpu-resource-summaries.md)):
- `gpu.resources` (`read`, no path) is registered by crd-perf-gpu-bridge, not crd-gpu-context: crd-gpu-context may
  not link crd-perf, and the bridge is the one module that names both. The host owns a `GpuResourcesCommand` and
  registers the contexts and frame graphs it wants summarized (bounded at eight of each); ceridc binds it with none.
- It answers only evidence that exists: the process-wide identity registry's live resource, program and pass counts
  (one index space for every backend); per registered context its backend, adapter, validity and each validation
  mode's state (`active` or the reason it is off); per registered frame graph its last build's transient bytes after
  and before aliasing, budget refusal, and its last execute's barrier, submit, pass, async-pass and present counts and
  timing support. Heap usage is an `unavailable` item per context because no backend queries device heap usage or
  budget; a host that registered no context or frame graph gets one `unavailable` item saying so.
- Frame-graph counters are not synchronized with the graph's build and execute: the host registers a graph only when
  it calls the service from the thread that drives the graph. Context reads are immutable and the registry locks.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8c-program-inspect-under-execute.md)):
- Running an authored program under a debug session is its own authority class, `execute`: it is not a snapshot of
  evidence that already exists (`read`), and no other class implies it. (The row's "inspect" authority is `read`;
  the class that may run code under a session is `execute`.)
- A request may carry named arguments (`DiagArg`: a `[a-z0-9_]` name and a byte-string value). Only a command
  registered with an argument check takes them. The service bounds their count, name and value sizes in the bounds
  step (`oversized`) and, in the arguments step, refuses a malformed or repeated name, any argument to a command
  without a check, and whatever the command's check refuses (`bad-argument`), all before the cursor and cancel
  checks. The check only parses; the command's handler parses the same values again to keep them. Like the path, the
  arguments only shape a new snapshot; later pages are cut from the retained one.
- `program.inspect` (`execute`, a path and named arguments) is registered by crd-ceir-cook beside `program.provenance`.
  The program is a CEIR text cooked under the request's relative path; the arguments are the script (entry, i64
  arguments, breakpoint lines, watched lines, steps, a stop bound up to the host's limit). One engine
  (`crd/ceir/cook/inspect_script.hpp`) runs the script on an `InspectHost` for both this command and `ceridc inspect`;
  the command answers a `breakpoint` item per breakpoint, a `stop` item per stop (sequence, reason, authored
  file:line:col, depth, op, action) followed by one `value` item per watched line, and a `result` item per result, so
  a stop with many watches never clips a value. The run happens inside the request on the calling thread as the
  controller, with every wait bounded by the host's `wait_ms`; the caller's cancel flag is polled while the program
  runs and at every stop, and a cancel that arrives before the executing thread attached is repeated until the session
  takes it. A caller cancel answers `cancelled`; a run that does not end within the bound answers `failed`.
- The agent transport reaches it only through the `diag` tool (`args`: an object of strings), under the grant the
  process started with (`ceridc mcp --diag-grant read,execute`); `ceridc diag --param name=value` is the CLI form.
  `ceridc inspect` stays the command line's convenience report over the same engine (any readable path, one
  document) and is not an MCP tool.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8c-replay-preparation.md)):
- Replay preparation is `replay.prepare` (`read`, a path), registered by crd-ceir-cook beside `program.provenance` and
  loading a program exactly as it does (one private loader now serves both). It prepares and never runs: for one
  authored program it answers what a replay of a run would need and which inputs exist. It is not a recorder, a recipe
  format or a bundle section; those belong to DIAG.9a/9b, and bundles carry no program or input sections to read.
- One item per input in a fixed order, tagged with the DIAG.9a guarantee it serves: `program` and `build` (identity),
  `entry-arguments`, `random` (RandomRead), `clock` (TimeRead), `host-state` (the host and world `*Read` families),
  `external-results` (FileIO, NetworkIO, DeviceIO, ExternalCall, AgentAction) (event), `schedule` (Synchronization,
  Nondeterministic) and `device-tolerance` (GPUCommand: a declared tolerance or oracle, never a bit-identity claim).
- Needs come from each op's effective effects: a `func.call` resolves through the module's symbol table to its
  callee's, with a cycle guard per op, so an effect inside a callee is charged to every call that reaches it and the
  item blames the first op in pre-order at its authored file:line:col. Empty is not unknown: an unregistered op, or one
  whose effects include ExternalCall, is opaque; it needs external results, and makes every other effect-derived input
  `unknown` (reported missing) rather than not needed.
- Only the program's content hash exists (and a cooked file's recorded one); every other needed or unknown input is
  missing with its precise reason, and the summary says `replay: unavailable` and lists them. The summary also gives
  the weakest determinism class the registered ops claim and how many make no claim.

Settled (2026-10-07, [session](../sessions/2026-10-07-diag-8c-gui-consumer.md)):
- The GUI consumer is crd-perf-ui's `DiagCommandPanel` (`crd/perf/ui/diag_panel.hpp`), beside the profiler panel: a
  form over the service's own request (a command from the listing, a path, `name=value` argument lines, page bounds)
  and a view of its own response document. It sends `DiagCommandService::execute` the request and keeps the returned
  `DiagResult` unchanged; it adds no command, check or authority, and the grant is the host's, fixed when it built the
  service. The panel's only checks are its own form: no command selected, a field longer than the panel's buffer
  (larger than the service's bounds, so an oversized path or argument still reaches the service's refusal), and an
  argument line without `=`. Everything else is the service's refusal, shown with its reason.
- The frame never waits. A request runs on the panel's own worker thread, because a command may hold the service for
  a bounded run (`program.inspect`); the frame's `tick` polls for the answer, and the command listing is copied when
  the panel is built (and on a refresh while idle), so a frame never takes the service's lock. One request runs at a
  time (`Busy`). `cancel` raises the flag the running request was given, which the service and the handler poll, so
  the GUI can end a running request (the synchronous MCP stdio loop still cannot). Destroying the panel cancels and
  joins it.
- Paging: a submit takes a new snapshot; the next page sends the snapshot's own request with the last page's next
  cursor, whatever the form holds now; any cursor can be sent, so an old snapshot's cursor shows the service's
  `stale-cursor` refusal. Items are shown one per row by splitting the document's `items` array structurally (strings
  skipped with their escapes), never re-serialized.
- Because the worker calls the service, a host must not register state that only one thread may read with a service
  it gives the panel: crd-sandbox registers its GPU context with `gpu.resources` (immutable reads) but no frame graph.
  crd-sandbox builds the service from start-up flags only (`--diag-grant`, default `read`; `--diag-root`; amended
  2026-10-12 by DIAG.11a: the default grant is now the diagnostic-host profile's `command_grant`), serves the
  ceridc set (the built-ins, `program.provenance`, `program.inspect`, `replay.prepare`, `gpu.resources`), draws the
  panel next to the profiler, and `--diag <command>` (with `--diag-path`, `--diag-param`) sends one request at start
  and logs every page; an unknown command refuses to run.

<a id="diag-9a"></a>
## DIAG.9a — reproducible inputs and determinism envelopes

Record build/configuration, assets/cooked hashes, random streams, initial state, clock/time-step inputs, input events,
external I/O completions and relevant schedule choices at production boundaries. Define event replay, task schedule
replay and backend-specific numerical replay as different guarantees. Store immutable artifacts or mark missing inputs;
do not assume the current checkout reproduces an older captured generation.

Acceptance: reproduce a seeded authored-runtime failure after process restart and asset edits, detect first divergent
state/event, and reject incompatible replay explicitly. GPU nondeterminism uses a declared tolerance/oracle; no claim
of bit identity across all hardware. Network/physical effects are stubbed only in explicit test replay, never silently rerun.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-run-records-and-replay.md)):
- A run record (`crd/ceir/cook/replay_record.hpp`, crd-ceir-cook) is an immutable, versioned (`kReplayRecordSchema`),
  checksummed file holding the build that made it (version, platform, compiler, arch, debug or release, asserts, and
  `kReplayExecutor`, the compiled-plan semantics version), the program as its cooked CRDR blob with its content hash
  and the authored path it was cooked under, the entry and its arguments, every replay input's need and state, a
  bounded trace and the outcome (run error, the faulting op's stable id, results, state cells). Encoding is
  deterministic: one run gives the same bytes. Decoding bounds every count and size before use and executes nothing.
- The trace is taken at the compiled-plan executor's safe points (`plan::RunControl`): one event per dispatched instr
  (op stable id, call depth), with up to four results read at the next safe point of the same frame through
  `plan::read_value`. An instr whose results are not readable there (the last instr of a loop body before its back
  edge, the last before a return) carries no values, and that is recorded and compared as is. `max_events` (default
  4096, at most 65536) bounds the kept events; `events_total` counts all, so loss is stated.
- Input states come from the same effect analysis as `replay.prepare` (one private walk now serves both): the
  program, build and entry arguments are recorded; an effect-derived input the program needs (or may need, through an
  opaque op) is stored `missing`, never assumed. The plan executor has no random, clock, host-state or external-I/O
  op today, so nothing captures those inputs yet.
- Replay loads the record's own blob into a fresh Context (never the checkout's file), recompiles the recorded entry,
  runs the recorded arguments with the recorded bound and reports the first divergence in a fixed order: path (op or
  depth of an event), value (a result, or the number of results read), length (event count), outcome (error or
  faulting op), results, cells. Replaying the same inputs against another program (an edited checkout) is a separate,
  explicit request that reports whether the program matches the recorded content.
- Incompatible replay is refused before anything runs: not a record, another schema, a short file or bad checksum, a
  blob whose recomputed content hash is not the recorded one (`failed`); a record from another build (`unavailable`,
  naming the differing fields, unless the request says `build=any`) or one missing an input its program needs
  (`unavailable`, naming the inputs).
- Commands: `replay.record` (`execute` and `record`: the service's `also` class; a program path; `out`, `entry`,
  `args`, `max_events`) creates the record exclusively under the root, refusing an existing file before it reads or
  runs anything; `replay.run` (`execute`; a record path; `program`, `build`) answers the divergence and both outcomes
  at their authored positions; `replay.prepare` now also reads a record, answering its build and arguments as
  available. ceridc's CLI and MCP services and crd-sandbox bind them.
- Guarantees: this is event replay of the integer, sequential compiled-plan executor. Schedule replay (pooled and
  parallel work), backend-specific numeric replay with a declared tolerance, recorded random, clock, host-state and
  external-completion streams, asset generations of a hot-reloading host and capture at the interactive hosts'
  boundaries (InspectHost, crd-sandbox frame loop, HostProvider) are not covered by this batch.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-inspect-host-recording.md)):
- Capture at the inspect host's boundary. `inspect::Session::run` takes an optional safe-point observer, called on
  the executing thread at every safe point before the session decides whether to stop (so it sees the instr a stop
  holds); a `Cancel` it returns ends the run. `ReplayRecorder` (`replay_record.hpp`) is the trace `run_traced` takes,
  exposed so a host can pass it to a session as that observer; a debugger's stops, steps and value reads therefore
  leave the trace exactly as an unobserved run's.
- `InspectHost::start(args, HostRecording)` records an execution: on the controller, before the executing thread
  starts, it re-cooks the installed generation's module in its own Context into the record's blob (valid only when it
  cooks to that generation's own content hash) and classifies the inputs with the same walk as `replay.record`; the
  executing thread traces into the host's execution allocator. `InspectHost::record` gives the record once the
  execution has ended (`running` before; `cancelled` for a cancelled run, which a replay would not stop where it
  stopped; `not-recorded` after a start without recording). A run never spans a reload (a load is refused while an
  execution is attached), so one record names one generation, and after a reload it still holds the generation that
  ran.
- Record schema 2 adds the program's asset id and ReloadSet generation (0 and 0 when no reloading host ran it; a
  generation without an asset is malformed); a schema 1 record is refused `unsupported-schema`. `replay.run` answers
  both. `write_record_file` creates a record exclusively (never overwrites).
- Consumers: `ceridc inspect --record <file>` writes the inspected run's record (an existing file is refused before
  anything runs; a cancelled run writes nothing), which `ceridc diag --command replay.run` reproduces in another
  process. crd-sandbox's `--inspect-record <file>` records the run its frame loop holds and steps through the panel
  (`InspectPanel::start` passes `HostRecording` to the host) and writes it when the run ends; "Run again" starts
  without recording.
- What stayed open after this batch is listed at the end of the next settled block.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-host-provider-records.md)):
- Measured first: the host provider builds a fresh submitting interpreter for every `execute` (no state cell
  survives one), and no production host keeps a reference interpreter across invokes. Its only schedule choices a
  program can observe are its settings: the job split, which results never depend on (bodies are state-free by
  pre-flight, outputs are index-ordered, folds run in index order, the lowest failing index wins), and the per-body
  step budget, which decides whether a body exhausts its fuel. `async.race` answers its first operand.
- A record names its executor (schema 3: `ReplayExecutorKind`, `plan` or `host`). A host record also holds the job
  split and per-body step budget and the interpreter's run error (`exec::ExecError`); the decoder refuses a host
  record without a valid schedule or with a plan error, and a plan record with either host field (`malformed`).
  Schema 1 and 2 records are refused `unsupported-schema`. A record replays only on its own executor: `replay.run`
  refused a host record `unavailable` before anything ran (until a host executor was bound, next block); the host
  replay refuses a plan record.
- `cook::InterpreterRecorder` is the host trace: the submitting interpreter's pre hook appends an event (op stable id,
  call depth) and its post hook, which runs only after a successful dispatch, reads up to four results right after
  the op ran (the plan trace reads them at the frame's next safe point, so the two traces are never compared). It
  keeps the state cells' current values in stable id order when it detaches. `HostProvider::execute(..., observer)`
  (`HostObserver`) attaches it to the submitting interpreter before the entry runs and detaches it after every pooled
  launch was joined. Step hooks are not copied to sub-interpreters, so parallel ranges, fold steps and pooled launch
  bodies leave no events; what they produce is read at the op that joins them (the await, the reduction).
- `crd/ceir/host/host_replay.hpp` (crd-ceir-host, now linking crd-ceir-cook; cook stays jobs-free):
  `record_host_run` runs a cooked program once on a fresh provider with a `HostSchedule` and makes the record (the
  schedule input `recorded`, the program, build and arguments recorded, any other needed input `missing`);
  `replay_host_record` refuses a plan record, another build (unless `any_build`), a missing input or a blob that is
  not its recorded content before anything runs, then replays the record's own blob in a fresh Context with the
  recorded job split and step budget and reports the first divergence at its authored position (found by stable id
  in the module, `replay_site_in_module`). Another program or another job split is an explicit option.
- What stayed open after this batch is listed at the end of the next settled block.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-host-record-commands.md)):
- Host records through the replay commands. crd-ceir-cook stays jobs-free, so it cannot run the host provider: it
  declares a host executor table (`cook::ReplayHostExecutor` in `replay_diag.hpp`: a record and a replay function
  over plain request and answer types, `HostRecordRequest`, `HostReplayRequest`, `HostReplayAnswer` with owned
  positions, `cook::OwnedReplaySite`) and `ReplayCommands::host` points at one. crd-ceir-host provides it
  (`crd/ceir/host/host_replay_diag.hpp`, `host_replay_executor()`), backed by `record_host_run` and
  `replay_host_record`, so a command answers exactly what the library calls do. With none bound, `executor=host` and
  a host record are refused `unavailable` before a byte is read or anything runs.
- `replay.record` gains `executor` (`plan` by default, or `host`), `jobs` (1 to 256, default 8) and `sub_fuel` (1 to
  2^32, default 2^20); the schedule arguments without `executor=host` are `bad-argument` in the service's argument
  step. The program is cooked exactly as for a plan record, then run once on a fresh host provider; the summary names
  the executor and the schedule, and the run's error is the interpreter's (`exec_error_name`).
- `replay.run` dispatches on the record's executor after the shared checks (decode, build unless `build=any`,
  missing inputs); a host record then goes to the host executor, which also refuses a blob that is not its recorded
  content. `jobs=` replays a host record on another split (`bad-argument` for a plan record). The answer has the same
  items as a plan replay: the divergence at its authored position in the replayed program, and the recorded and
  replayed outcomes (the recorded fault now located in the record's own program, `HostReplay::recorded_fault`).
- The host provider takes its parallel ranges' job arrays from the calling thread's frame arena, so it must run on a
  thread the pool enrolled (the thread that called `jobs::init`, or a worker). ceridc's `diag` and `mcp` verbs own a
  4-thread pool for their length and the MCP loop resets the arenas after each request; ceridc registers the task and
  async dialects with the scalar subset. crd-sandbox does not bind the host executor: its diagnostic panel calls the
  service from its own unenrolled worker thread, so its replay commands refuse host records `unavailable`.
- The committed `assets/ceir/host_replay_demo.ceir` (a pooled launch calling a function, a map_reduce, a state cell
  and a loop stepped by the argument) fails `bad-for-step` on `main(0)`; its record made by one `ceridc` process is
  byte-equal to the native record, reproduces in a second process after the launched constant is edited, and a third
  names the await reading it (recorded 25, observed 36).
- What stayed open after this batch is listed at the end of the next settled block.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-inspected-host-records.md)):
- A host run recorded while an inspection session drives it. A session installs its own step hooks on the
  interpreter it runs, which replaced a recorder's. `inspect::Session::invoke` takes a `StepObserver` (a pre and a
  post hook and their user): the session's pre hook calls the observer's at every safe point before it decides whether
  to stop there (so it sees the op a stop holds, as the plan form's observer does), and its post hook forwards the
  observer's. `exec::Interpreter` exposes its installed hooks (`pre_hook`, `post_hook`, `hook_user`), so
  `HostProvider::execute(..., session, observer)` hands the hooks the `HostObserver` attached to the session; detached
  bodies are unchanged (no hooks of either kind).
- `host::HostProgram` loads a cooked program into its own Context (dialects registered, stable ids assigned, the
  artifact copied), so a host binds its session to the module that runs (the module form of `Session::bind`).
  `record_host_run(program, entry, args, schedule, max_events, session, ...)` runs it once on a fresh provider under
  the session (or none) and makes the record; the blob form is a wrapper over it, so both make the same bytes. The run
  comes first: a run the session cancelled is refused `cancelled` (`HostReplayStatus::Cancelled`, the host executor's
  `failed`) and leaves the record and the missing-input list untouched.
- A controller's stops, steps, step into a called function and out, and value reads leave the record byte-equal to an
  unobserved run's; the stepped frame's events carry its depth and values; a breakpoint in a pooled body is counted
  and refused `DetachedBody`, never paused, and leaves no event.
- No interactive consumer inspects host runs yet (ceridc inspect, `program.inspect` and the sandbox panel drive the
  compiled plan through `InspectHost`), so this is a library capability with its tests.
- What stayed open after this batch is listed at the end of the next settled block.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-host-input-seam.md)):
- A host input seam. A program reads values the host chooses at run time through the `input` dialect
  (`engine/execution/ceir/ops/input.ceirop.toml`); `input.random {stream, bound}` is its first op: one raw 64-bit
  draw of host random stream `stream` (in [0, 2^32)), reduced by the op to [0, `bound`) (unsigned remainder; `bound`
  in [1, 2^32)). It declares `RandomRead` (so the replay-input walk classifies it with no special case), claims
  `ExternalNondeterminism` and is a native intrinsic of the `host` provider. No clock, time-step, input-event or
  external-completion op is specified anywhere yet, so none was added: each needs its own specification (units,
  domains) first.
- `crd/ceir/input.hpp` (crd-ceir) defines `InputKind` (stored in records; append only), `InputSource` (one `next`
  call per read on the executing thread, in program order, false when the host has no value), `read_input`,
  `reads_input`, `random_attrs`, `reduce_draw` and `SeededInputs`, a source whose stream `s`'s draw `n` is a
  splitmix64 mix of (seed, s, n): independent streams, the same draws for one seed, not cryptographic.
- Both executors read through the seam: the reference interpreter's `install_input_semantics` (a separate installer,
  like async and task) with `Interpreter::set_input_source` (not copied to sub-interpreters), and the compiled plan's
  `Op::Random` (stream and bound packed in the immediate) with `plan::run(..., inputs)`. With no value the read fails
  `ExecError::InputUnavailable` / `RunError::InputUnavailable` at the op, never a made-up value; an out-of-range
  attribute is `UndefinedValue` / `BadConst` (the `arith.const` precedent). A read is schedule-dependent like a §20
  cell, so `exec::region_state_free` refuses it: a parallel or map_reduce body (or a callee it reaches) that reads one
  is `ParallelBodyStateful` / `ParallelStateful`, and a launch body that reads one is not pool-eligible and runs inline
  on the submitting interpreter, which has the source. `HostProvider::set_input_source` hands the source to its
  submitting interpreter.
- Record schema 4 holds the host input reads after the cells: every read's kind, channel, whether the host had a
  value and the raw value, the first `kReplayMaxInputReads` (65536) kept and all counted; the decoder bounds the
  count and refuses an unknown kind or a value the host never delivered; schema 3 is refused. A recorded run's reads
  go through `InputRecorder`, which wraps the host's live source; a replay installs `InputFeed` over the record's reads
  and never asks a live source. A read the record cannot answer (another kind or channel at its position, or past its
  last read) fails `InputUnavailable` there and is noted with the trace event of the op that asked.
- `first_divergence` reports in run order: the kept events up to the op whose read was refused, then that refusal
  (`input`: the recorded and requested channels, or the read counts when the record had none left), then the rest
  of the events, length, a different number of reads (`input`, counts), outcome, results and cells.
- `random` is recorded only when the record holds every read the run made (`host_inputs_held`: not past the bound)
  and every op that itself declares `RandomRead` is an input op reading through the seam; otherwise it is missing and
  a replay refuses the record. `replay.record` takes `seed=` (a u64: the run's `SeededInputs`; without it the host has
  no random source, and the failed read is what the record holds) and answers `random_source`, `seed` and
  `input_reads`; `replay.run` answers `recorded_input_reads` and `replayed_input_reads`. The host executor takes the
  seed through `HostRecordRequest`, and `record_host_run` / `run_host_traced` take the live source; host replays feed
  the record's reads on any job split. The inspect host installed no source and kept no reads, so a program that drew
  failed `input-unavailable` there and its record stored `random` missing (superseded by the next block).
- ceridc, crd-sandbox's diagnostic service and its inspect panel register the dialect. The committed
  `assets/ceir/random_demo.ceir` draws `main(n)` values from stream 0, switches on each (a draw of 3 has no case) and
  returns a stream-1 draw; a seeded failure recorded by one `ceridc` process reproduces in another without the seed,
  after the program file was edited.
- What stayed open after this batch is listed at the end of the next settled block.

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-inspect-host-inputs.md)):
- A live input source at the inspect host. `inspect::Session::run` (the plan form with an observer) takes the run's
  `inputs` and passes them to `plan::run`. `InspectHost::start(args, recording, inputs)` takes the caller's source,
  read on the executing thread; it must stay valid until the execution has ended, so a caller declares it before the
  host (`run_inspect_script` can return `Unfinished` with the run attached, and the host's destructor cancels and joins
  it). It runs concurrently with the controller, so it must not share the controller's allocator: a `SeededInputs`
  grows its per-stream counters on first use, so each consumer gives it its own `GrowableTlsfAllocator` (the TSan lane
  caught the shared form: the controller's report allocation raced the executing thread's first draw).
  `InspectScript::inputs` carries it to the scripted consumers.
- A recorded inspected execution always reads through an `InputRecorder` (over the source, or none), so its record
  keeps every read, delivered or not, as `replay.record` does: a stepped, held and value-read seeded run records the
  same reads, inputs, trace and outcome as the unobserved `replay.record seed=` run, and with no source the failed read
  is the record (`random` recorded, as `replay.record` without a seed). `random` is classified held at `start`; once
  the run has ended, `record` stores it missing when more reads were made than a record keeps (`kReplayMaxInputReads`).
- `SeededInputs::reset(seed)` starts every stream over at draw 0, so one source can serve successive runs.
- Consumers: `ceridc inspect --seed <u64>` (a strict decimal u64, refused before anything runs; the report names
  `random_source`, `seed` and the record's `input_reads`); `program.inspect seed=` (a u64, checked in the service's
  argument step; the summary answers `random_source` and `seed`); crd-sandbox's `InspectPanel::set_inputs` and
  `--inspect-seed <u64>`: the panel owns a `SeededInputs` and its allocator (before its host) and resets it at every
  start, so "Run again" reads the same draws from the first.
- What stayed open after this batch is listed at the end of the next settled block (the clock and time-step inputs
  it named are settled there).

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-clock-inputs.md)):
- Two `input` ops, specified in `input.ceirop.toml`: `input.clock {domain}` reads a time domain's current reading and
  `input.time_step {domain}` the length of its current step. `domain` must name one of the time dialect's six
  built-in domains (`time::builtin_domain_index`: wall 0, sim 1, frame 2, audio_sample 3, sequencer 4, logical 5, an
  append-only order stored in records); anything else is `UndefinedValue` at eval and `BadConst` at plan compile. The
  value is the host's raw i64, unreduced: nanoseconds for wall and sim, ticks of the domain otherwise. The wall domain
  is the host's monotonic clock from the clock's own epoch, never calendar time, so a record names no date. Both ops
  declare `TimeRead` (a pure read in the hazard classification) and `ExternalNondeterminism`.
- Read order: the seam's read order is the executors' program order. Both executors run a block in list order and no
  pass reorders ops between cook and execution today (the cook runs no transform, and nothing on the host
  executors' path reorders by the hazard analysis), and a replay runs the record's own blob, so the order a record keeps is the order it feeds back. A pass
  that reorders time reads would have to keep that order or bump `kReplayExecutor`.
- Seam: `InputKind::Clock` and `InputKind::TimeStep` (appended; the channel is the domain ordinal). `input::HostClock`
  answers the readings and steps the host set (`set_reading`, `set_step`, `advance`, `clear`) and, with
  `use_live_wall(reader, user)`, the wall domain live from the host's monotonic reader (crd-ceir links no clock;
  crd-ceir-cook's `monotonic_ns` reads crd-time's `MonotonicClock`). `input::InputRouter` sends each kind to its own
  source. Neither allocates, so either can serve the inspect host's executing thread. Both executors read the ops
  through the seam (`Op::Clock` and `Op::TimeStep` in the plan); the parallel-body pre-flight refuses them like
  `input.random`, and a pooled launch body that reads one runs inline.
- Records: the schema stays 4 (no layout changed; a kind past the last is still refused, and a time read of a channel
  past the built-in domains is now refused as malformed). `clock` is recorded under the same rule as `random`: every
  read held and every op declaring `TimeRead` reading through the seam; otherwise it is missing and a replay refuses
  the record (the inspect host's bound applies to both). An `input` divergence also names the recorded and the
  requested kind (`recorded_input`, `observed_input`), since a clock read and a step read of one domain share its
  channel.
- `replay.record` takes `clock=wall` (the live wall), `sim_time=` and `sim_step=` (i64 nanoseconds) on both executors
  (`HostClockSpec` on `HostRecordRequest`); the summary answers `wall_clock`, `sim_time`, `sim_time_ns`, `sim_step`
  and `sim_step_ns`. The other domains have no reading on that one-shot host. The committed
  `assets/ceir/clock_demo.ceir` fails when the sim step is over 33,333,333 ns and otherwise returns the wall reading
  plus the sim reading plus its argument: a failure recorded by one `ceridc` process reproduces in another with no
  clock, and a live-wall result replays to the recorded value.
- What stayed open after this batch is listed at the end of the next settled block (the clock at the interactive
  hosts it named is settled there).

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-interactive-host-clock.md)):
- A clock at the interactive hosts. One parser serves every one-shot host's clock arguments,
  `cook::parse_clock_argument` (`replay_diag.hpp`): `clock` (only `wall`), `sim_time` and `sim_step` (decimal i64
  nanoseconds) give `NotClock`, `Ok` (set in a `HostClockSpec`) or `Refused` with what the value must be.
  `replay.record` now uses it too, so its refusals are unchanged.
- One input bundle serves the interactive hosts, `cook::RunInputs`: a `SeededInputs` on its own allocator (the inspect
  host's threading rule), a `HostClock` and an `InputRouter` sending Random to the seeded source (or none) and Clock
  and TimeStep to the clock. `set(seeded, seed, spec)` starts the next run over: every stream from draw 0, the clock
  from no reading (a live wall's epoch is that call). `clock()` lets a host set more domains between runs. The host
  keeps it alive until the run has ended, so it is declared before the host. A host with no seed and no clock reads
  through it exactly as through no source: every read fails `input-unavailable` and a record keeps the failed read.
- Consumers: `program.inspect clock= sim_time= sim_step=` (checked in the service's argument step; the summary
  answers `wall_clock`, `sim_time`, `sim_time_ns`, `sim_step` and `sim_step_ns` after `seed`, the fields
  `replay.record` answers, so three existing summary assertions were updated); `ceridc inspect --clock wall
  --sim-time <ns> --sim-step <ns>` (`InspectClockFlags`; refused before anything runs; the report names the same five
  fields); crd-sandbox's `--inspect-clock`, `--inspect-sim-time` and `--inspect-sim-step` (`PanelInputs::clock`,
  applied at every `start`, "Run again" included).
- The sandbox frame loop's clock. `--inspect-frame-clock` (`PanelInputs::frame_clock`) gives a run the frame loop's
  clock as it is at the run's start: the sim domain reads the loop's time and its last frame step in nanoseconds (the
  frame counter's under `--fixed-dt`, so deterministic there), the frame domain reads the frame index with a step of
  one frame. The loop hands the panel its clock every frame (`set_frame_clock`), and the panel copies it into the
  run's clock only in `start`, on the controller thread before the run's thread exists. The clock never changes while
  a run reads it, so a run held across frames keeps reading its start's frame clock; a run that read a live frame
  clock would need a per-domain reader that is safe to call from the executing thread, which nothing needs yet. The
  first run starts before the first frame (time 0, frame 0, the fixed step or 0). It replaces the spec's sim domain;
  the sandbox refuses it together with `--inspect-sim-time` or `--inspect-sim-step`.
- What stayed open after this batch is listed at the end of the next settled block (input events are settled there).

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-input-events.md)):
- Input events. `input.event {queue}` (specified in `input.ceirop.toml`) takes the next event of host input event
  queue `queue` (in [0, 2^32); out of range is `UndefinedValue` at eval and `BadConst` at plan compile). The seam
  delivers one raw packed i64 per read (`InputKind::Event`, appended; the channel is the queue) and the op unpacks it
  into five results: bits 0..7 the type (0 none, 1 key_down, 2 key_up, 3 key_repeat, 4 mouse_down, 5 mouse_up,
  6 mouse_move, 7 scroll, 8 resize: `input::EventType`, the platform's `InputEvent::Type` order, append-only), 8..23
  the key or button, 24..31 the modifiers (shift 1, ctrl 2, alt 4, super 8), 32..47 x and 48..63 y as signed 16-bit
  values (the pointer position in whole pixels, the scroll offset in hundredths of a step, the new size; 0 otherwise).
  The host adapter quantizes: CEIR values are integers, so the packed form is what a record keeps and a replay feeds.
  An open queue with nothing left delivers 0, a `none` event (an empty queue is a value, not a missing input); a queue
  the host does not have is no value and the read fails `input-unavailable`.
- Effects `UIRead` and `UIWrite`: taking an event reads the host's UI input and consumes it, so the hazard analysis
  orders two reads like two random draws (RandomRead writes its class for the same reason). `ExternalNondeterminism`,
  native provider host. Both executors read it in program order (`Op::Event`, the queue in the immediate; the plan
  writes the five results through the instr's result slots); the parallel-body pre-flight refuses it.
- `input::HostEvents` holds up to 16 queues and 4,096 events; a read takes its queue's next event in push order and
  never allocates (`open` and `push` grow on the host's allocator, between runs); `rewind` and `clear` start over.
- Records stay schema 4. `UIRead` was already one of `host-state`'s families, so the input is now a seam input under
  `random`'s rule (`is_seam_input`, also in the inspect host's bound): recorded when every read is held and every op
  that declares a host-state family itself reads through the seam. A scene, ECS, physics, audio, document, constraint
  or other UI read keeps `host-state` missing and the replay refused. Its missing reason became "the host-state reads
  are not held; replay.record keeps only input.event reads" (the `replay.prepare` expectation was updated). The trace
  keeps four results per event, so an event read's fifth result (y) is never compared in the trace; it is a fixed
  function of the recorded raw read, which the replay feeds back unchanged.
- `replay.record events=` (both executors; `HostEventsSpec` on `HostRecordRequest`; `cook::parse_events_argument`,
  `cook::apply_events`) opens queue 0 holding at most 32 comma-separated events: `key_down`, `key_up`, `key_repeat`,
  `mouse_down` or `mouse_up:<code>[:<mods>]`, `mouse_move`, `scroll` or `resize:<x>:<y>`, every field range-checked
  and no field left over. An empty value is an open, empty queue; without the argument the host has no queue. The
  summary answers `event_queue` (open or none) and `input_events`. The committed `assets/ceir/event_demo.ceir` takes
  two events and fails `selector-out-of-range` when the second is a resize; otherwise it returns the first event's
  code plus the second's x and y plus its argument.
- What stayed open after this batch is listed at the end of the next settled block (input events at the interactive
  hosts are settled there).

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-interactive-host-events.md)):
- Input events at the interactive hosts. `cook::RunInputs` also owns an `input::HostEvents` on its own allocator,
  routed for `InputKind::Event`; `set(seeded, seed, clock, events)` applies a `HostEventsSpec` through
  `apply_events`, so every call starts the queue over (no spec: no queue), and `events()` lets a host queue more
  between runs. `program.inspect events=` (checked in the service's argument step with `parse_events_argument`; the
  summary answers `event_queue` and `input_events` after the clock fields, so three existing summary assertions were
  updated) and `ceridc inspect --events <list>` (refused before anything runs; the report names the same two fields)
  give a run the listed events on queue 0, as `replay.record events=` does.
- crd-sandbox's panel has two exclusive sources for queue 0 (the sandbox refuses both together): `--inspect-events`
  (`PanelInputs::events`, copied by `set_inputs`, every run from the first event) and `--inspect-window-events`
  (`PanelInputs::window_events`). The application drains the window's `platform::InputEvent` queue itself and
  dispatches typed events through its layer stack, so the sandbox pushes `sandbox::InspectEventLayer`, which rebuilds
  each key, mouse button, pointer, scroll and resize event as the platform event it came from and never marks it
  handled. `sandbox::window_event` is the host adapter's quantization: the type in the platform's order (each value
  `static_assert`ed against `input::EventType`), the key or button as its platform enum value, the modifiers as
  shift 1, ctrl 2, alt 4 and super 8, and x and y saturated to 16 signed bits: the pointer rounded half away from zero
  to whole pixels, the scroll offset in hundredths of a step, the new size in pixels (a NaN is 0).
- Window events are staged, never written into a live queue. The panel keeps them on the frame-loop thread (at most
  `kMaxWindowEvents`, the queue's own 4,096; past it the newest replaces the oldest and the dropped are counted);
  `start` copies them, oldest first, into an open queue before the run's thread exists, and they are taken only when
  the start succeeds (a `Busy` start keeps them). Events arriving while a run is held feed the next run. The first
  run starts before the first frame, so its queue is open and empty; "Run again" takes what arrived since. The record
  keeps every read, so it replays from its reads alone.
- What stayed open after this batch is listed at the end of the next settled block (device numeric replay is
  settled there).

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-device-records.md)):
- Backend-specific numeric replay of compute dispatches, the acceptance criterion's "declared tolerance/oracle". A
  device record is a run record of a third executor, `ReplayExecutorKind::Device` (schema 5; schema 4 is refused
  `unsupported-schema`). It holds no trace: it holds the adapter that ran it (backend, name, vendor, device, driver and
  API versions, as the executor reports them), the declared envelope, the CKIR text of every dispatched kernel with
  its FNV-1a 64 hash, each declared buffer's initial contents and, for the buffers a dispatch writes, their contents
  after the run, and the executor's error code and blamed dispatch. Its `entry-arguments` are the initial contents and
  its `device-tolerance` input is recorded (the envelope). The decoder refuses device fields in another executor's
  record, a device record with an entry, trace, results or input reads, an exact envelope with a bound, a bound past
  2^24 ULP, outputs that are not exactly the written buffers' and more than 16 kernels, 16 buffers, 1 MiB of kernel
  text or 2^22 elements.
- A device program is a cooked module whose one top-level block holds only `arith.const`, `resource.declare` of a
  plain f32, i32 or u32 buffer, and `compute.dispatch` over a positive constant grid binding buffers declared before
  it, with a `kernel` symbol and one `r`, `w` or `rw` access per binding; anything else is refused `unsupported`
  naming the op. The envelope is declared by whoever records, never inferred: `exact` (every bit pattern equal) is
  claimed only on the recording adapter and build, so a replay on another adapter is refused `other-adapter` naming the
  differing fields, and on another build `other-build` even when another build is allowed, both before anything runs;
  `ulp:N` holds f32 outputs within N representable values (both zeros 0 apart, a NaN matching only a NaN, one NaN
  incomparable) and integer outputs equal, on any adapter, and replays across builds only when asked. Bit identity
  across hardware is never claimed.
- `crd/ceir/cook/device_replay.hpp` (crd-ceir-cook, which now links crd-kir): `record_device_run` and
  `replay_device_record` take a `DeviceExecutor` (a run function, its user pointer and its adapter); crd-ceir-cook
  still names no GPU API. `reference_device_executor` is the CPU reference: crd-kir's `eval_cpu_kernel`, the oracle
  the GPU backends are proven against, over each dispatch in block order (f32 widened to f64 and narrowed after each
  dispatch, integers exact). It refuses, rather than evaluate wrongly, a kernel that is not a scalar compute kernel
  with a one-dimensional workgroup and grid or that reads a builtin other than `LocalInvocationIndex` and
  `WorkgroupIndex`. A replay loads the record's own blob, refuses a blob or kernel text that is not its recorded
  content (`content-mismatch`) and a missing input, runs the recorded initial contents, then compares the executor's
  outcome first and every written element in declaration and element order. The first element outside the envelope is
  the divergence: buffer, element, both bit patterns, the distance and the bound, the buffer's declaration and the
  last dispatch writing it at their authored positions. `max_distance` reports the largest f32 distance over every
  element. Replaying against another program or other kernel text is an explicit option, reported as such.
- The GPU side stays a caller hook. `gpu::execute_lowered_host` (crd-ceir-gpu) runs a lowered list on host data:
  one device-local buffer per binding, `validate_lowered` before anything is recorded, upload with the CKIR harness's
  barriers, `execute_lowered`, read back the written buffers, submit and wait; a binding without data or a buffer the
  device refuses is the new `ExecuteError::HostTransfer`. `IComputeContext::adapter()` (appended, default unknown)
  reports `ComputeAdapter`; the Vulkan context answers from its physical device's properties, the DX12 context from
  its device's DXGI adapter (driver: the user-mode driver version). The tests' GPU executor lowers the block,
  compiles each kernel's CKIR text for its backend and reports the context's adapter.
- Measured on this machine (RTX 4070 Ti SUPER): the fixture's `exp(b) * a` over 256 elements recorded on Vulkan and
  on DX12 replays bit-identically from its file on its own adapter, is refused on the CPU reference under `exact`,
  and differs from the CPU reference by at most 3 ULP on both backends, inside the declared 32 (a budget from Vulkan's
  3 + 2|x| ULP exp bound and D3D's relative bound, not a measurement); declared one ULP below the measured distance it
  diverges at `@wave`'s dispatch. lavapipe (llvmpipe, LLVM 20.1.2) differs by at most 4 ULP; WARP was not
  measured.
- What stayed open after this batch is listed at the end of the next settled block (the device executor bound to the
  replay commands is settled there, apart from crd-sandbox).

Settled (2026-10-08, [session](../sessions/2026-10-08-diag-9a-device-record-commands.md)):
- The replay commands reach a device executor: `ReplayCommands::device` is the host's `cook::DeviceExecutor` (null:
  `replay.record executor=device` and a device record's `replay.run` are refused `unavailable`, the record request
  before anything is read). The executor runs on the request's thread, so a host binds one only where that thread may
  use it.
- `replay.record executor=device` takes the run from files under the host's root, because an argument holds at most
  512 bytes: `envelope` (`exact` or `ulp:N`, required, declared and never inferred), `kernel_dir` (kernel `@k`'s CKIR
  text is `<kernel_dir>/k.ckir`, each at most 1 MiB) and `buffers` (one little-endian 32-bit file per declared buffer,
  in declaration order, at least one element each and 2^22 over all, every file bounded by what is left of that
  budget before a byte of it is read). The run arguments of the other executors (`entry`, `args`, `max_events`,
  `seed`, `clock`, `sim_time`, `sim_step`, `events`) are refused with it and the three device arguments without it,
  in the argument step. `describe_device_program` lists the kernels a device program dispatches (each once, in first
  dispatch order) and the buffers it declares; every file is read and checked before `record_device_run` runs the
  executor, and the record is the library's record of the same artifact, inputs and envelope, byte for byte.
- `replay.run` replays a device record with `replay_device_record` (its build, adapter and input checks before
  anything runs; an exact record is claimed only on its own adapter and build, even with `build=any`); `program=`
  replays it against another program and `kernel_dir=` against the kernel texts in a folder (an edited checkout).
  The answer names the first element outside the envelope with both bit patterns, distance, bound, the last dispatch
  writing it and the buffer's declaration at their authored positions, both adapters and `max_distance`.
- ceridc binds the CPU reference (`reference_device_executor`) as its device executor: it opens no GPU context, so an
  exact GPU record is refused there and an ulp record is checked against the reference within its envelope. A record
  made by one ceridc process reproduces in another after a kernel file is edited, and the edit is named at its
  dispatch.
- crd-ceir-gpu's `run_device_block` (`crd/ceir/gpu/device_run.hpp`) is the GPU half of a device executor: lower the
  block, read and compile each dispatched kernel with the host's `KernelCompileFn`, run `execute_lowered_host`, report
  the blamed dispatch. It names no record type, so crd-ceir-gpu still does not link the cook bridge; a GPU host wraps
  it with its compute context's adapter. The Vulkan and DX12 gates bind that executor to the commands: on this
  machine's RTX 4070 Ti SUPER a command-made ulp record is the library's record byte for byte, reproduces on the
  device and replays on the CPU reference at the library's measured distance.
- What stayed open after this batch is listed at the end of the next settled block (the sandbox's device executor is
  settled there).

Settled (2026-10-09, [session](../sessions/2026-10-09-diag-9a-sandbox-device-records.md)):
- crd-sandbox binds a GPU device executor to its replay commands. `GpuDeviceExecutor` (`sandbox/src`) is a compute
  context on the sandbox's backend, `run_device_block` over it and the context's own adapter: on Vulkan a
  `VulkanComputeContext` over the sandbox's own device, on DX12 a `Dx12ComputeContext` (its own D3D12 device and
  compute queue on the default adapter). Kernels compile through the concrete contexts' new
  `create_pipeline_from_ckir` (crd-kir's compute-kernel emitter, then SPIR-V or HLSL), which the Vulkan and DX12 gates
  now use too. With no valid context the executor is not bound and device requests are refused `unavailable`.
- The panel's requests run on its worker thread, so every device run is handed to the frame-loop thread:
  `FrameThreadCalls` holds one posted call at a time, the worker blocks in `call` until the frame loop's `pump` (once
  per frame, before the panel's tick) has run it, and `FrameThreadDevice` wraps the executor so its runs go through
  it with the same adapter. A run therefore never submits from the worker, and it holds the frame that pumps it for its
  bounded length. The frame loop closes the calls when it ends, before the panel joins its worker (an explicit close in
  the teardown, and a guard declared after the panel for every other return path): the posted run and every later one
  are refused (`failed`, "the frame loop has ended"), nothing runs and nothing is written. DX12 goes through the same
  hand-over although its queue is its own, so one contract covers both backends.
- Measured on this machine (RTX 4070 Ti SUPER): the sandbox records the fixture program under `ulp:32` through its
  panel on Vulkan and on DX12; each record reproduces in a second sandbox process on its own backend (distance 0), the
  Vulkan record reproduces on the DX12 sandbox (another adapter, distance 0), and ceridc replays both on the CPU
  reference at 3 ULP. A device record is now replayed on a GPU in another process.
- What stayed open after this batch is listed at the end of the next settled block (the sandbox's host records are
  settled there).

Settled (2026-10-09, [session](../sessions/2026-10-09-diag-9a-sandbox-host-records.md)):
- `cook::ReplayHostExecutor` carries a `user` pointer, passed to its `record` and `replay` as their last argument
  (the executor's own state, distinct from the requests' `user`, which is the registrar's). The host provider's table
  is stateless (`user` null), so ceridc and the other hosts that bind `host_replay_executor()` are unchanged.
- crd-sandbox binds the host provider's executor to its replay commands through `FrameThreadHost`, over the same
  `FrameThreadCalls` as the device executor, so a host run and a device run never overlap. The panel's worker thread
  has no crd::jobs frame arena, and the host provider's parallel ranges take their job arrays from the calling
  thread's arena, so a host run on the worker is not allowed (a debug build asserts there). The wrapper hands each
  `record` and `replay` to the frame-loop thread, the thread that initialised the pool. That thread never resets its
  arena, so the wrapper takes the arena's mark before the inner call and puts it back after: a run's job arrays are
  dead once it returns, and runs do not accumulate in the arena. A refused call (the frame loop has ended) is
  `failed` with "the frame loop has ended: the host run did not start", and nothing is recorded. Teardown is the
  device executor's: the calls close before the panel joins its worker, and the wrapper dies after the commands and
  before the jobs pool.
- The sandbox's dialect registrar moved to `sandbox/src/sandbox_dialects.cpp` and gained `task` and `async`, the host
  provider's dialects. Without them, a host program such as `assets/ceir/host_replay_demo.ceir` did not load in the
  sandbox. crd-sandbox-inspect-tests compiles that source, so its panel cases load programs with the sandbox's own
  registrar.
- Measured on this machine: the real sandbox records `host_replay_demo.ceir` through `--diag replay.record
  executor=host`. A second sandbox process reproduces the record, and ceridc reproduces it as well. A host record is
  now made and replayed by a GUI host whose requests come from a worker thread.
- Still open in DIAG.9a, waiting on a consumer or an op (no local work can do these honestly yet): state cells across
  invokes and a run spanning a reload wait for a host that keeps one interpreter across invokes (only tests call
  `migrate_state` today); external I/O completions and network or physical effects stubbed only in explicit test
  replay wait for an op that declares `FileIO`, `NetworkIO`, `DeviceIO`, `ExternalCall`, `AgentAction` or a physics
  write (none does). Until then a program that needs external results keeps `external-results` missing and its
  record is refused before anything runs (`test_replay_diag.cpp`, `test_replay_record.cpp`).

<a id="diag-9b"></a>
## DIAG.9b — triggerable flight recording and fault injection

Add bounded pre/post-trigger recording for assertions, allocation failures, frame hitches, queue starvation and
resource pressure; retain loss metadata. Support test-only seeded allocator failure, delayed/cancelled completion,
partial I/O, device-loss simulation and reload interruption through existing seams. Capture the trigger and causal
inputs even when the faulty operation cannot complete.

Acceptance: each trigger yields evidence from before the event, bounded post-window or crash truncation, and a
replay/minimization recipe. Inject during load/cook/install/execute/reload/teardown; previous working assets survive
recoverable failure. Shipping capability checks reject injection and arbitrary external I/O replay.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-flight-recorder.md)):
- The flight recorder is `crd::perf::FlightRecorder` (`crd/perf/flight_recorder.hpp`): its events are fixed 152-byte records (code, severity, redaction class, two scalars, a
  message clipped to 111 bytes with the clip flagged), so recording and triggering never allocate. Codes 0x0900..0x09FF belong to it.
- Bounds are an authored, versioned `FlightPolicy` (capacity at most 65536 events, about 9.5 MiB; `pre_events`,
  `post_events`, `post_window_ns` at most 60 s, a trigger mask, `hitch_threshold_ns`). `pre_events + post_events <=
  capacity`, so the post window can never overwrite the pre window it keeps. A rejected candidate (or a reload while a
  capture is pending, or a generation that would wrap) leaves the active policy, its generation and the ring as they
  were. Defaults: 1024 events (152 KiB), 768 before, 256 after, 2 s, every trigger, 50 ms hitch.
- A trigger takes only its own mutex, never the collection lock, and a record takes none (DIAG.11b, 2026-10-24). The
  first trigger opens the post window; any trigger while a capture is pending is suppressed and counted with its kind,
  never replacing it. The window closes when
  `post_events` land, when `post_window_ns` elapses (seen by `poll()`), at shutdown or on the fatal path; the last two
  mark the post window truncated. Events between the window closing and collection are not recorded (the ring stays
  frozen) and are counted.
- A capture carries the trigger record (kind, code, the sequence it fired at, time, two scalars, file:line, message),
  the policy and its generation, the retained events in sequence order, the loss metadata (`recorded_before_trigger`,
  `pre_retained`, `post_retained`, `post_truncated`, `ring_unavailable`, `dropped_while_complete`,
  `suppressed_triggers` with their kinds) and the replay recipe registered when the trigger fired, or the reason
  replay is unavailable ("no replay recipe was registered with the flight recorder" by default).
- Collection: `poll()` delivers to the host's sink, `take_capture()` without one, `finish()` closes early. A capture
  that ended Fatal is terminal: the recorder records and delivers nothing more, so the second fatal path of one failure
  (`check_invariant`'s default reaches `CRD_FATAL`, which reaches the assert trigger) cannot replace it. On the fatal
  path the collection lock is taken with a bounded try-lock and claims still publishing are waited for within the same
  bound; if either runs out, the capture carries the trigger alone and says `ring_unavailable`. Building a capture allocates, so the fatal path is an ordinary handler call, never a signal
  handler: a hard signal crash still leaves only the DIAG.5a/5b crash record.
- The capture is bundle section `FlightRecord` (tag 6), a versioned little-endian payload ('CFR1') whose decoder
  bounds every length by its fixed limit and by the payload, checks the loss metadata against the event list and
  refuses trailing bytes; `write_flight_bundle` writes it beside a Manifest. `flight_capture_summary_json` is the
  one-line summary.
- Trigger sources (`crd/perf/flight_triggers.hpp`) sit on seams that already existed and change none of them:
  assertions (`crd::set_assert_handler`, chained to the previous handler; when the assert path will terminate,
  headless with no platform handler, the window is closed Fatal and delivered first), fatal invariants (new
  `get_fatal_invariant_handler` and `default_fatal_invariant`, chained the same way), queue starvation and hangs
  (`flight_on_starvation` / `flight_on_hang` for the jobs watchdog's handlers), frame hitches (`FlightFrameWatch`,
  fed by the host's frame boundaries), allocation failure (`FlightAllocatorWatch`, the `IRefusalObserver` of one
  allocator since DIAG.11b, an `IAllocator` decorator before: each refused request triggers; a fatal-on-OOM `allocate`
  reports before its own `CRD_FATAL`, which then closes the window through the assert trigger) and resource pressure (`FlightPressurePolicy`, an
  `IResidencyPolicy` decorator that triggers on each `StreamingAllocator` eviction request).

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-fault-injection.md)):
- Fault injection is `crd::perf::FaultInjector` (`crd/perf/fault_injection.hpp`) over a validated, versioned
  `FaultPlan`: a seed and up to 8 rules (site, first hit, hit count or unbounded, probability in parts per million,
  value), with a plain-text form that round-trips (`seed=7;allocation-failure@2+1%1000000;partial-read@0+0%250000=16`).
  Whether hit n of a site fires is a pure function of seed, site, rule and n (`fault_plan_fires`), so the same plan
  over the same call sequence injects the same faults. Arming restarts every site's hit count.
- Sites are existing seams with existing failure paths: `allocation-failure` (`FaultAllocator`, an `IAllocator`
  decorator; only `try_allocate` fails, since `allocate` is non-null or fatal), and four read sites on the I/O fault
  seam: `failed-read`, `partial-read` (deliver n bytes), `delayed-completion` and `cancelled-completion` (the last two
  only at an `AsyncFile` completion, the engine's one asynchronous completion; crd-jobs has no cancellation). A read
  counts a hit at every site it is eligible for and applies the first that fires (failed, partial, cancelled,
  delayed).
- The I/O fault seam is crd-core's `crd/core/fault_seam.hpp`: one hook, consulted by `read_file_binary`,
  `read_file_range` and the `AsyncFile` completion job; with no hook it is one atomic load; uninstall waits for calls
  in progress. A synchronous read fails on Fail, Short or Cancel, as a real failed or short read does.
- `AsyncFile::read_async(offset, dst, status)` reports `Complete`, `Short`, `Failed` or `Cancelled`; there is no
  status-less form. Before this a failed or short completion silently zero-filled `dst` and the streamed load handed
  those bytes to the loader; now any status but Complete fails the load before the loader runs.
- Evidence: a fired fault is recorded into the attached flight recorder (`kFlightCodeFaultInjected`, a = site,
  b = hit) and, unless the plan says `notrigger`, registers its recipe and fires an `InjectedFault` trigger (kind 9).
  The recipe `fault-plan/1 fired=<site>#<hit> minimal=<one-hit plan> plan=<armed plan>` names the one-hit plan first,
  so even a clipped recipe (marked `...`) reproduces the fault alone (minimization by construction). While armed, the
  plan owns the recorder's recipe: `arm` registers `fault-plan/1 plan=<text>`, each fired fault its own, and `disarm`
  marks replay unavailable until the host registers a recipe again. Any plan's text fits 800 bytes unclipped.
- Shipping checks: `arm` needs `DiagAuthority::Inject` in the caller's grant (refused Unauthorized first, as the
  command service orders it) and a build with fault seams. A shipping build (`CRD_SHIPPING`) compiles the seam out
  (`CRD_FAULT_SEAMS`, private to crd-core, from the top-level `_crd_shipping` condition, so native multi-config
  profiles keep working): every arm is Unavailable, the hook cannot be installed and every read and allocation runs
  plain. `fault.arm` and `fault.disarm` declare Inject; `fault.status` (Read) reports `compiled`.
- Recovery shown at the resource manager: a reload interrupted by an allocation failure in the real MESH loader, a
  failed pack read or a short pack read keeps the last good payload, generation and bytes; a streamed load whose
  completion is short, failed or cancelled ends Failed without reaching the loader; a delayed one completes.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-device-loss.md)):
- Device-loss simulation is the `device-loss` fault site on a second crd-core seam, the GPU fault seam
  (`install_gpu_fault_hook` / `consult_gpu_fault` in `crd/core/fault_seam.hpp`, compiled out with the I/O seam in a
  shipping build). Only a queue submission is consulted (`GpuFaultOp::Submit`), before any of its work is queued, and
  only on a device not already lost or removed, so each hit ordinal is one live submission. A loss is never injected
  at a wait: Vulkan cannot stop queued work, and a caller told its wait failed would free memory that work still
  uses. `FaultInjector::gpu_hook` decides one hit per consult and answers `LoseDevice` when it fires.
- Each provider then takes its own existing loss path, labelled Simulated, never presented as a real loss. Vulkan's
  `vk_submit` records `VK_ERROR_DEVICE_LOST` with origin and loss origin `Simulated`, so the fault report, the
  DIAG.7c(g) loss bundle and the lost-device short cut follow; the injection is consulted outside the completion
  seam's lock. DX12's `dx12_submit` calls `ID3D12Device5::RemoveDevice`, so every later call meets a removed device
  as after a real loss, reads DRED and records the removal with the new origin `Dx12RemovalOrigin::Simulated`; the
  removal bundle is version 3, since a version-2 reader does not know that origin.
- Recovery from a device loss is a new device: the lost context latches invalid and submits nothing more, and a
  context on a fresh device (the removed DX12 device is released with its context) submits and completes.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-assets-commands-doctor.md)):
- The flight policy and fault plans are authored TOML documents on the existing config path (crd-toml), parsed by
  `crd/perf/diag_assets.hpp`: `kind` (`flight-policy` or `fault-plan`) and `schema_version = 1` first, every other key
  optional with the in-code default. A fault plan's `rules` are strings in the plan-text grammar; the document is
  assembled into plan text and read by `parse_fault_plan`, so the asset and `fault.arm plan=` share one reader (a rule
  with `;` or without `@` is refused). An unknown key, a wrong type, a negative or oversized integer, an unknown
  trigger, a missing or other kind or version, invalid TOML, and a policy or plan its own validator refuses are all
  refused with the key named and the output untouched. An asset file is at most 16 KiB. The committed
  `assets/diag/flight_default.flight.toml` is held equal to `FlightPolicy{}`.
- `FlightCommandHost` (`crd/perf/flight_commands.hpp`) is the recorder's sink: once a sink is set a capture is handed
  over and `take_capture` finds nothing, so the host keeps a copy of the last capture (under its own mutex) and passes
  each one to an optional forward sink. `flight.status` (Read) answers the recorder's state and active policy and the
  retained capture, with its events as items; `flight.trigger` (Record) fires a manual trigger; `flight.policy`
  (Record, request path) reloads the policy from an asset under the root, a refused document, file or reload keeping
  the active generation; `flight.write` (Record, request path) polls, then writes the retained capture as a flight
  bundle, never over an existing file.
- `fault.arm` also takes `asset=<path>`, a fault-plan asset at a safe relative path under the host's root (Unavailable
  without one); it is the route for a plan longer than the 512-byte argument bound.
- The doctor reports two live modes from `DoctorLive` (and `diag.capabilities` from `DiagServiceConfig`'s
  `flight_recorder` and `fault_injector`): `flight-recorder` (compiled; enabled when initialised; usable unless a
  capture ended Fatal) and `fault-injection` (compiled only with fault seams; enabled when armed; usable when compiled
  and installed). The default argument keeps every earlier call's report as it was, plus the two modes.
- Still open in this row: the build and symbol identity sections in a flight bundle; injection during cook, install,
  execute and teardown; refusal of arbitrary external I/O replay as a shipping capability check (a record needing
  external results is already refused before it runs, DIAG.9a); and a real host (crd-sandbox) installing the recorder,
  its triggers, a `FlightCommandHost` sink and the flight and fault commands.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-flight-bundle-identity.md)):
- A flight bundle names the build and the binaries that wrote it: `write_flight_bundle(capture, modules)` writes the
  Manifest, the FlightRecord, a `BuildIdentity` section (tag 7) and a SymbolIndex (tag 5) of the host's module
  snapshot; with no snapshot the SymbolIndex is an explicit absent section the manifest declares. Writing never
  enumerates modules, so the fatal path writes a snapshot taken earlier.
- The build identity (`crd/perf/build_identity.hpp`) is compile-time constants only: the diagnostic schema, the engine
  version, platform, compiler and its version, arch, `debug`/`release`, and a closed flag set (asserts, profiling,
  fault seams, ASan, TSan). Its payload ('CBI1', version 1, six fields of at most 64 bytes) is refused on a wrong
  magic or version, unknown flags, an over-long or short field or trailing bytes. `same_build_identity` names every
  differing field; `bundle.inspect` reports the bundle's build with `same_as_host` and `differing`, so a reader can
  refuse a replay recipe recorded by another build.
- The module snapshot comes from `enumerate_loaded_modules` (`crd/perf/symbol_index.hpp`), the live counterpart of the
  DIAG.5d minidump parser with the same identities: on Windows a Toolhelp snapshot, each module pinned while its
  in-memory PE headers are read within SizeOfImage (TimeDateStamp, SizeOfImage, CheckSum, the RSDS GUID and age); on
  Linux `dl_iterate_phdr` into preallocated slots (the PT_LOAD span and the GNU build-id). Only basenames are kept,
  module and PDB, and Windows names compare without case (the loader keeps the case a module was loaded by, a dump the
  mapped file's). At most 1024 modules, with an overflow flag. It takes the loader lock and allocates: hosts call it
  at start and after loading modules, never on a fatal path or in a loader callback.
- `merge_module_snapshot` keeps a module that unloads, marked unloaded, so a bundle still names the binary a capture's
  events came from; a module loaded again is a new generation. `FlightCommandHost::refresh_modules` keeps the host's
  snapshot and `flight.write` refreshes it before writing.
- `import_bundle` decodes tag 7 (`has_build`, or `build_refused`) and does not count an explicit absent SymbolIndex as
  symbols. The bundle fuzz target's oracle covers tag 7 and its corpus holds a BuildIdentity bundle and a refused one.
- Still open in this row: injection during cook, install, execute and teardown; refusal of arbitrary external I/O
  replay as a shipping capability check; and a real host (crd-sandbox) installing the recorder, its triggers, a
  `FlightCommandHost` sink with its module snapshot and the flight and fault commands.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-cook-faults.md)):
- The cook stage is the asset processor (crd-cooker's `cmd_cook`), the one step that turns sources into the packs a
  host loads. Its reads are already on the I/O fault seam, and `read_file_text` now consults it too (a `ReadWhole`
  consult, like `read_file_binary`), so a cook's `.meta` sidecars and database, configuration and authored render
  assets can be failed, cut short or delayed. The injector's flight event for a read names the file it failed, by its
  last path component only ("injected failed-read at hit 4 (seed 11) on a.bin"); a host's directories never reach the
  evidence, and a name past the event's bound is clipped and flagged.
- Unreadable is not absent. Absence stays a recorded dependency, but a file that exists and cannot be read (a failed
  or short read, a sharing violation) is not cooked as if it were missing: `CookIO` keeps the first such input
  (`unreadable()`), and the processor publishes nothing for that run whatever the handler returned. An asset's `.meta`
  that cannot be read stops the cook and keeps the asset's id (only a `.meta` whose text holds no id is regenerated),
  and `CookIO::stable_id` never mints over a sidecar that exists but cannot be read. Before this a transient read
  failure of a `.meta` or id sidecar gave the asset or sub-product a new identity, breaking every reference to it, and
  an optional input that failed to read was cooked as absent and published.
- A cook that stops publishes nothing: the previous pack, the cached artifacts, the cook database and every id are
  kept byte for byte, a job that had begun leaves its journal `begin` dangling so the next run distrusts and recooks
  it, and the next clean cook publishes the new content under the same id. A read fault in the up-to-date check (an
  input or a cached artifact that looks absent or torn) recooks the job and publishes the same bytes; it never ships
  a stale product. Shown with the recipe's one-hit plan reproducing each interrupted cook alone.
- Still open in this row: injection during install, execute and teardown; refusal of arbitrary external I/O replay
  as a shipping capability check; and a real host (crd-sandbox) installing the recorder, its triggers, a
  `FlightCommandHost` sink with its module snapshot and the flight and fault commands.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-install-teardown-faults.md)):
- Install has two forms, both on the I/O fault seam already. A cooked pack is installed into the running resource
  manager by `mount_manifest` (a patch or DLC pack over the mounted ones; the newest mount serves an id). A cooked
  render asset is installed into the renderer's live slot by `SceneRenderer::reload` (RAF-11: the resolver reads the
  source with `read_file_text`, the reloader stages, and the install commits at a frame boundary and bumps the
  generation). An install interrupted by a failed or short read installs nothing: no mount is added and the mounted
  pack keeps serving the id with its loaded payload's pointer, generation and bytes; the renderer keeps its live frame
  and generation (a clean reload of the old source afterwards is a no-op). The capture names the file, and the
  recipe's one-hit plan interrupts the same install alone.
- A pack torn on disk is refused at install. `mount_manifest` validated the manifest container but not the entries'
  byte ranges, so a pack cut inside its artifacts mounted, its entries shadowed the earlier mount's and every later
  load of those ids failed. A pack whose manifest names bytes past its own end is now refused whole, before the live
  table is touched (the hot-reload path already skipped such an entry).
- Execute is the device-loss slice above: a fault injected at a live queue submission is injection while work
  executes, with recovery on a fresh device. No CPU execution path has a recoverable read or allocation seam today;
  the DX12 frame descriptor arena's `try_allocate` is the candidate if one is wanted.
- Teardown has no read or recoverable allocation of its own, and that is checked rather than assumed: with a plan
  that fails every read and every `try_allocate` armed and hooked, dropping the handles and destroying the resource
  manager consults no site. After a run in which every fault kind fired, the teardown returns every allocation the
  run made (counted through the manager's and the payloads' allocator, including a payload retired by a reload and
  still in its grace period), the host's required drain waits out a delayed completion still in flight, and the
  recorder's open window is delivered at shutdown marked truncated, with the later faults counted as suppressed.
- Still open in this row: refusal of arbitrary external I/O replay as a shipping capability check, and a real host
  (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink with its module snapshot and the
  flight and fault commands.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-external-io-replay.md)):
- No build replays external I/O. A run record has no section for external I/O completions or opaque call results,
  and nothing can feed them to a replay but bytes from outside the run, so the refusal is not a shipping-only switch:
  every build refuses, and a shipping build is checked the same way.
- The decision comes from the program, never from the record. `check_external_replay` (crd-ceir-cook,
  `replay_record.hpp`) walks the ops of the program a replay would run with the `replay.prepare` effect walk; a
  program needing external results (an op whose effective effects include FileIO, NetworkIO, DeviceIO, ExternalCall
  or AgentAction, or an opaque op) is refused Unavailable before anything runs, naming the program, the number of
  such ops and the first at its authored file:line:col. `replay.run` asks it on every executor (plan, host, device)
  for the record's own program after its content hash is checked and for the `program=` program before its plan is
  compiled; the host path asks before the host executor is called, so a host executor never sees such a program.
  Before this a record that said `external-results` was recorded, or said it was not needed while its program read
  a file, replayed; the checksum guards integrity, not what the record claims.
- `record_missing_inputs` counts `external-results` missing unless the record says it is not needed and not held, and
  `replay.prepare` never answers it available from a record. (2026-10-10, DIAG.10a: a record file that says it holds
  external results, or whose state contradicts its need, is now refused as it is decoded; `record_missing_inputs`
  remains the guard for records held in memory, and the DIAG.9b cases check both.)
- The doctor reports `external-io-replay` never compiled, enabled or usable, in any build, beside `fault-injection`
  (compiled out of shipping builds); `diag.capabilities` carries both.
- Today no engine op declares an external-results effect, so such a program would also fail the plan compiler
  (`unsupported-op`); the refusal is decided before that, so it holds when an op that reads a file gains a kernel.
  Stubbed external completions stay as DIAG.9a records them: only in an explicit test replay, once such an op exists.
- Still open in this row: a real host (crd-sandbox) installing the recorder, its triggers, a `FlightCommandHost` sink
  with its module snapshot and the flight and fault commands.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9b-sandbox-flight-host.md)):
- crd-sandbox is the first real host (`sandbox/src/flight_host.{hpp,cpp}`, `FlightHost`). Every run starts the
  recorder before the diagnostic service and the window, with the default policy (`FlightPolicy{}`, held equal to the
  committed default asset) or an authored one (`--flight-policy <asset>`; a refused asset or a missing capture
  directory refuses the run, as an asked-for and unavailable option does elsewhere in the sandbox).
- Its trigger sources: assertions and fatal invariants (chained to the handlers before them); the jobs watchdog's
  starvation and hang reports, with a 250 ms watchdog window the host gives a pool that has none (without a window the
  handlers never fire); frame hitches, fed from the top of each frame-loop iteration (start to start, so no `continue`
  skips a frame); and allocation failure at the resource manager's allocator. The sandbox has no `StreamingAllocator`,
  so the resource-pressure trigger has no seam in this host.
- The resource manager allocates through `FaultAllocator` over the arena, whose refusals `FlightAllocatorWatch`
  observes: an injected failure is one injected-fault capture (the arena never sees the injected request, so nothing
  is suppressed), and a real failure of the arena an allocation-failure capture.
- The recorder's sink is a `FlightCommandHost` whose module snapshot is taken at start and refreshed once the GPU
  device is up (its driver's modules load then). With `--flight-dir <dir>` every delivered capture is also written as
  `flight-<n>.cdb` (the first free n, never over an existing file; the newest `kDefaultMaxBundles` are kept). The
  writer can run on a fatal path, so it takes its own lock with a bounded try that refuses at once on the thread
  already inside it, reads the snapshot with the new `FlightCommandHost::try_modules` (never waited on: a thread inside
  a snapshot update, or one that cannot get it in a bounded number of tries, writes the bundle with its module list
  declared absent), logs nothing and calls neither the recorder nor a command service. Captures are logged from the
  frame loop.
- With `--diag` the service also serves `flight.*` and `fault.*`, its doctor reports the live recorder and injector,
  and the injector's I/O and GPU fault hooks are installed (refused in a shipping build, where every arm answers
  Unavailable; arming still needs `inject` in the grant).
- Teardown undoes it in order after the jobs pool is shut down: the hooks (uninstall waits for calls in progress), the
  plan, the jobs handlers, the assertion and fatal triggers, then the recorder, which delivers and writes a pending
  capture. The watchdog is a thread, so a return path that skips the ordered teardown still shuts the pool down. A
  smoke failure now leaves through the ordered teardown with exit 2: an injected device loss reached that path, and
  leaving through the scope destroyed the device before its children (`VUID-vkDestroyDevice-device-05137`) and then
  crashed.
- `flight.status` on a capture whose trigger message and recipe are both at their bounds keeps every field; the two
  strings are cut at the 160-byte field bound (which alone marks the summary clipped) and the one-hit plan that
  reproduces the fault survives the cut whole, so no field order changed.
- Amended 2026-10-12 (DIAG.11a, [session](../sessions/2026-10-12-diag-11a-reusable-diagnostic-host.md)): the sandbox's
  `FlightHost` became the engine's `crd::perf::DiagnosticHost` (`crd/perf/diagnostic_host.hpp`) and
  `sandbox/src/flight_host.*` is deleted; its cases run in crd-perf-tests. The sandbox now starts it from a
  diagnostic-host profile (`--diag-host`, default `assets/diag/host_default.host.toml`), which also owns the crash
  handler; frames are fed by `Application::tick`. `fault.*` and the seams' hooks are served only when the profile turns
  fault injection on (`host_investigate.host.toml` does) and the build has the seams; a shipping build reports
  injection Unavailable and serves no `fault.*`. `--flight-policy`, `--flight-dir` and `--diag-grant` remain as
  overrides of the profile, held to its rules. See [DIAG.11a](#diag-11a).
- With this every local clause of the row is shown; it waits only for its hosted run.

<a id="diag-9c"></a>
## DIAG.9c — rare-failure escalation and minimization

Provide verified rr/TTD recipes on eligible hosts, including tool/version/permission checks, symbols, original exit
codes, storage bounds and offline replay. Add seed/choice/input minimization around the Cerid reproduction harness.
Preserve natural uninstrumented failures alongside instrumented traces to detect observer effects. Hardware watchpoints
and external sampling are documented bounded options; no machine-wide security downgrade or compulsory service.

Acceptance: a small intentional corruption is found by replay/backward inspection on each claimed tool route; a
missing tool or incomplete recording is instrument failure. A real-multicore test remains separate from rr's serialized
execution. Platforms without reverse-debugger support retain event/schedule evidence and an explicit capability limit.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9c-reverse-debug-routes.md)):
- The intentional corruption is `crd-diag-corruption-specimen` (`tests/support/diag/specimens/corruption_specimen.cpp`,
  in the default build on every lane, debug information requested): an off-by-one loop in `crd_diag_corrupt_neighbour`
  writes one word past four cells into a sentinel; bounded churn follows, then the check. `clean` mode runs the same
  code with the right bound. No library call runs inside the recorded window. It is single-threaded: process record
  and rr serialize execution, so the TSan and stress specimens stay the multicore evidence.
- `scripts/reverse-debug-recipes.py` runs each route and judges it. The specimen's uninstrumented run comes first and
  its outcome is kept: a corrupt run that does not detect the corruption is `SPECIMEN_INVALID`, and an instrumented
  run whose detection differs from it is `OBSERVER_EFFECT`. A route is `FOUND` only when it names
  `crd_diag_corrupt_neighbour` in the specimen's source, at the marked store for a reverse route (which stops before
  the store) or inside the writer for a forward watchpoint (which stops after it). A clean run with a writer is
  `FALSE_FIND`; a missing tool, a refused permission, an ineligible host, missing symbols, a recording that lost the
  write, a debugger internal error, a timeout and a trace over its byte bound each have their own failing verdict.
  The verdict record holds the tool and version, permission state, the natural and instrumented outcomes, the
  instruction bound and log size, the trace size and bound, and the writer's frame. gdb's debuginfod is turned off and
  cdb's symbol path is the specimen's directory, so no route reaches the network.
- Routes and their state on the reference hosts:
  - `gdb-record` (Linux, gdb 15.1): process record with a bounded instruction log, a live `reverse-continue` to a
    `watch -l` on the sentinel, then `record save` and an offline replay in a second gdb that restores the log and
    bisects instruction numbers with `record goto`. gdb 15.1 hits an internal error (`find_inferior_pid`) on
    `continue`, `reverse-continue` or a watchpoint over a restored log, so the offline leg only navigates and reads.
    Found live and offline at the store on linux-gcc-debug and linux-gcc-shipping. Not a route on sanitizer builds
    (declared at configure time): recording works, but `record save` never finishes writing the reserved shadow space.
  - `gdb-watch` (Linux): a forward hardware watchpoint; a software fallback is `HOST_INELIGIBLE`. Found on all four
    Linux lanes, sanitizer lanes included.
  - `cdb-watch` (Windows): cdb from the WinDbg package (not on PATH) with a hardware data breakpoint (`ba w8`) set at
    the image entry; first-chance access violations do not stop it, because MSVC's ASan commits its shadow through
    them. Found on win-debug, win-shipping, win-clang-cl-shipping and win-asan. Local evidence only: no hosted
    Windows job installs a debugger.
  - `ttd` (Windows): TTD.exe from the WinDbg package records, and cdb replays the trace stepping backwards (`g-`) to
    the data breakpoint. Recording needs an elevated shell; unelevated the launch is refused and reported
    `PERMISSION_DENIED`. Verified 2026-10-14 ([session](../sessions/2026-10-14-diag-9c-ttd-recording.md)): the
    elevated recordings answer `FOUND` at the corrupting store (the replay steps back one instruction past the data
    breakpoint's stop) and `CLEAN`; the standalone TTD package records, the one inside the WinDbg app cannot be
    launched, and `--replay-trace` re-judges a recording unelevated.
  - `rr` (Linux): needs hardware performance counters and `perf_event_paranoid <= 1`. WSL2 exposes no `cpu` PMU
    (rr 5.7.0: "Unable to open performance counter") and has paranoid 2; the route reports `HOST_INELIGIBLE` and never
    lowers the setting. The replay leg reuses the gdb-record driver and is unverified until an eligible host exists.
- Hardware watchpoints (`gdb-watch`, `cdb-watch`) are bounded options: a fixed number of debug registers, a forward
  stop only, no history. External sampling stays the DIAG.6c runbook (`scripts/sample-cpu-wpr.py`).

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9c-minimization.md)):
- The reproduction harness is the three reproducible artifacts the engine already makes: a schedule script (DIAG.4b,
  the choices), a fault plan (DIAG.9b, the seed and the faulting hits) and a run record (DIAG.9a, the inputs). Each is
  minimized against the caller's run of the reproduction, and nothing new is recorded to do it.
- `crd/core/minimize.hpp` (header-only, below every module that reproduces something, so the gate-ON schedcheck
  twin of crd-jobs can use it without linking crd-perf) holds the shared parts. The oracle is all K runs: a candidate
  counts only when every one of `Budget::runs_per_candidate` runs fails the original way; a candidate that fails in
  some runs and not in a later one is a race, counted in `Stats::flaky` and never kept. A candidate is never judged on
  fewer than K runs: when the run budget cannot hold K more, the helper stops with `Budget`, holding the smallest
  candidate judged so far, which reproduces but is not claimed minimal. Every helper judges its input first and
  answers `NotReproduced`, leaving it as given, when the input does not reproduce. `minimize_sequence` is the DIAG.4b
  greedy single-element remover promoted (order kept, the pass restarted after each drop, 1-minimal when a whole pass
  drops nothing; the reason it is not ddmin is kept in the header). `bisect_toward` moves a reproducing bound toward
  a non-reproducing one until they are adjacent; `minimize_toward_zero` tries 0, then bisects the magnitude keeping
  the sign. Bisection claims a local boundary when the predicate is not monotone, never the global one.
- Choices: the DIAG.4b schedule-script minimization now runs through `minimize_sequence` with K = 3, and still
  shrinks the recorded failing log to exactly `{test.reacquired, fp.finalizing}`; the choreography orders the
  re-acquire against the scheduler's owner-release check (`fp.checked`), so no visited candidate is a race.
- Seed and hits: `minimize_fault_plan` (`crd/perf/fault_minimize.hpp`, crd-perf) takes a plan that reproduces and the
  caller's run (arm the plan, run, report each site's hit count). It narrows every rule's range in rule order (an
  unbounded rule bounded first at the hits the last reproducing run counted; then its first hit raised and its last
  hit lowered by bisection, so moving either end one hit inward was judged not to reproduce); rule indices do not move
  in that step, so a seeded rule keeps firing on the same hits inside its range. It then pins the plan: the hits each
  rule fires on are enumerated with `fault_plan_fires`, and when there are at most 64 that fit in 8 rules, the plan of
  exactly those hits (each always firing, consecutive hits of one site and value merged, seed 0) replaces it if it
  reproduces, so the seed no longer matters. Last it removes single hits of a pinned plan (a candidate needing more
  than 8 rules is skipped, never run) or else whole rules, to a 1-minimal result. It never changes what a fault does,
  only which faults happen. It is pure apart from the caller's runs, so it runs in every build; a shipping run cannot
  arm, so a shipping reproduction answers `NotReproduced` with the plan unchanged.
- Inputs: `minimize_record_inputs` (`crd/ceir/cook/replay_minimize.hpp`, crd-ceir-cook) takes a plan record of a
  failed run. The failure is the recorded outcome: the same run error blamed on the same op, with every host input
  read answered by the stream. After judging the record's own inputs it cuts the read stream to the reads the failing
  run made, then shrinks each argument and each delivered read's value toward 0, passes repeating while a value moves;
  a value is shrunk again only when another value moved after it was last shrunk, so the passes settle. A read's
  position, kind, channel and delivery never change: those are the program's read sequence. The new record comes from
  one more run of the final inputs through the recorder `replay.record` uses, made by this build, and `replay.run`
  reproduces it like any other. Refused before anything runs, as `replay.run` refuses: not a plan record, an incomplete
  input stream, another build (unless `any_build`), a missing input, a program that is not the recorded content hash,
  and a program that needs external I/O results (DIAG.9b). A run that did not fail answers `NotAFailure`.
- Shown: a two-switch program whose argument fails at its first switch from 10 up (and below 0) and at its second for
  2 to 9 shrinks from 1000 to 10 (the second switch is another failure) and from -1000 to -1; a seeded
  `random_demo.ceir` run failing at draw k shrinks to `main(k + 1)`, its earlier draws to 0 and its failing draw to a
  value that still reduces to 3; a plan failing every allocation shrinks to the one needed hit; a seeded four-rule
  plan to the two needed hits on two sites, seed-free; a half-share plan armed on a real injector through a
  `FaultAllocator` to its two needed attempts; and a 40% read plan that fails the real cooker after the cook recovered
  from failures of its first two reads, to the one read that fails it (the `.meta` that holds the asset's id), each
  remaining hit checked needed by cooking without it.
- Not covered: a diagnostic command that runs a minimization (the helpers are library calls a host or test makes);
  minimizing the program text itself; host and device records (schedule replay and numeric replay have no shrinking
  defined); dropping or adding reads, which would change the program's read sequence.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-9c-observer-effect.md)):
- Instrumentation levels are off, basic and full. Off is no flight recorder at all: nothing records and no trigger
  fires, which is the natural, uninstrumented run. Basic and full are a `FlightPolicy::detail` (`FlightDetail`,
  authored as `detail = "full" | "basic"`, default full, the recorder's behaviour before levels existed). Basic records
  events of `Severity::Warning` and above only (anomalies, failures, injected faults); informational events (frame
  boundaries from `FlightFrameWatch`) are not recorded, are outside the sequence and are counted in
  `FlightRecorder::total_filtered()` (and `flight.status`'s `filtered`). Every trigger fires at both levels, so a hitch
  still opens a window and carries its interval in the trigger. A basic policy's left-out events are its declared
  scope, not loss: the capture's policy names its level, and `post_events` counts only kept events. The CFR1 record
  carries the level in the byte after its flags, which was a must-be-zero pad: every earlier record reads as full, a
  value naming no level is refused ("unknown detail level"), and a reader that predates a level refuses it rather
  than misreading it; the record version and the committed fuzz corpus are unchanged. The summary JSON and the
  policy asset name the level.
- An observer-effect experiment (`run_observer_experiment`, `crd/perf/observer_effect.hpp`, crd-perf) runs a
  reproduction K times at each level and compares the caller's failure signature (which op, site, hit or frame
  failed; never a time). The natural runs come first and are kept as `natural`, beside every capture: they must agree
  and fail (`not-reproduced` otherwise, `natural-unstable` when they disagree, since an observer cannot be judged
  against an unstable baseline). Each instrumented run gets a fresh recorder; any run whose outcome differs from the
  natural one, including a level that fails in only some runs, is `observer-effect` naming the level; a level that
  reproduced but whose run delivered no capture is `instrument-failure` (the instrument missed the failure it was
  attached to catch); a policy the recorder refuses runs nothing and is `instrument-failure`. Only `consistent` passes.
  Each level's run times are reported as its cost and judged by nothing. The recorder allocates its ring and captures
  from the default allocator, never through an allocator a run injects faults into, so an allocation-failure plan
  counts the same hits at every level by construction.
- Shown: a frame stream with one long gap (judged by its own 33 ms budget, observed through `FlightFrameWatch`, no
  fault seams, every lane) and an allocation-failure plan on a real `FaultInjector` behind a `FaultAllocator` are
  consistent: basic keeps the injected fault and leaves the frames out (counted), full keeps both, and basic's events
  are a subset of full's; the real cooker, failed by the first single read fault it does not recover from, is
  consistent with both captures naming the file (the cooker records nothing informational, so the levels keep the
  same events). A `notrigger` plan and a policy masking the hitch trigger are `instrument-failure`. The
  `observer-effect` verdict is shown on reproductions perturbed on purpose and labelled synthetic: an instrument that
  takes one attempt from the allocator under test (the fault moves to the frame before, at both levels), one that
  moves the gap at full only, and one whose instrumented runs pass one time in three. No real observer effect was found
  in the engine's instruments. A shipping build cannot arm a plan: those reproductions answer `not-reproduced` there.
- Found on the way: crd-toml took the last value of a key defined twice in one table, so a diagnostic asset with
  `capacity = 64` and later `capacity = 1` was read as 1. TOML forbids defining a key twice; crd-toml now refuses it
  ("duplicate key", at the second definition), in tables and inline tables. Every committed TOML file parses
  unchanged.
- Not covered: a diagnostic command that runs an experiment (it is a library call a host or test makes); experiments
  on schedule-script reproductions (the schedcheck twin then ran in no hosted job; it does since DIAG.11b); the
  profiler's scope trace as a third instrumented level (a process singleton, compiled out on the perf-off lane).

<a id="diag-10a"></a>
## DIAG.10a — stateful fuzzing and evidence-reader security

Extend the existing bounded fuzz framework to allocator operation sequences, capture/bundle/policy readers, current
command decoding, resource reload and compiled/reference CEIR comparisons. Use real production entry points with
memory/time/recursion limits and reference/metamorphic oracles. Register every new ingress surface with a fuzz owner;
future media/network/plugin formats keep corresponding product rows rather than pretending they exist now.

Acceptance: seed malformed lengths, overflow, cycles, truncated writes, invalid generations and destructive import
attempts. Minimize/adopt reproducible findings and replay corpora in relevant CI. Reinvestigate retained unknown-origin
artifacts under the strengthened harness without converting a clean replay into an invented explanation.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-10a-allocator-fuzzing.md)):
- Allocator operation sequences are one stateful fuzz target, `crd-fuzz-memory-allocators`
  (`tests/foundation/memory/fuzz`), on the existing harness: its corpus replays as a CTest on every lane and
  `linux-clang-fuzz` builds the libFuzzer executable. It covers TLSF, pool, linear, stack, the growable TLSF, pool and
  linear, the offset allocator, the epoch ring, the `DiagnosticAllocator` over TLSF and the generation-checked
  `SlotMap`. The virtual-memory and streaming allocators are not in it: each input would reserve address space.
  (2026-10-11: they are now, each input reserving at most 16 MiB and releasing it with the allocator; see the
  Settled block of that date below.)
- The production entry points get only operations their contract allows, so a failure is a defect and never harness
  undefined behaviour; malformed requests are inside that contract (every `try_allocate` refuses a zero size, a size
  no arena can hold, an alignment at the top bit and one that is not a power of two). Hostile operations go only to
  the layers that promise to diagnose them: the `DiagnosticAllocator` reports a double free, a foreign or interior
  free and a free of a block already evicted from quarantine, and finds a seeded rear-redzone overrun and a write
  into quarantine with exactly that violation; the `SlotMap` refuses stale, double-erased and forged handles and
  never revives one on slot reuse. Generation wrap (2^32 erases of one slot) is not reached by a bounded input.
- The oracles are an independent model, never the allocator's own report: no overlap, alignment, ownership,
  allocation size, intact payloads across every operation, exact success or refusal where the capacity model is
  exact (pool, linear, stack), `validate_structure()`, `MemoryStats::bytes_in_use` returning to zero (and equal to
  the arena's use for the bump allocators) where statistics are tracked, the offset span coalescing back to one
  region, the parent getting every byte back on destruction, and an identical outcome trace on a second run.
- Seven defects found and fixed, each kept as a corpus input. Five came from the written seeds: TLSF `try_allocate`
  asserted on a non-power-of-two alignment instead of refusing it; TLSF in-place reallocation never adjusted `bytes_in_use`, which drifted by every
  in-place resize; the linear, stack and growable linear allocators charged `size` but released the consumed
  offset, so alignment padding drove `bytes_in_use` past zero (they now charge what they consume, and
  `bytes_in_use` equals `used()`); the `DiagnosticAllocator` passed any pointer without a record to its backing
  allocator when the backing range check accepted it, so an interior pointer or a double free after quarantine
  eviction corrupted the backing heap (only a live untracked passthrough block goes through now; otherwise
  `UnknownPointer`); and its saturation report named the discarded redzoned block instead of the one handed out.
  Two came from libFuzzer on `linux-clang-fuzz` (ASan+UBSan): a TLSF pool whose capacity is not a multiple of 16 put
  its end sentinel at a misaligned address (`init_pool` now rounds the capacity down), and `OffsetAllocator::reset()`
  kept node neighbour links, so a whole-span free after a reset coalesced with regions that no longer existed
  (`reset()` now returns every node to its initial state). Two later libFuzzer reports were the harness's own:
  an offset span no size bin represents exactly can never be handed out whole, by the allocator's bounded
  internal-fragmentation design, so the drained span is compared with a fresh one; and while an untracked
  passthrough block is live the `DiagnosticAllocator` cannot tell an evicted pointer from it (the saturation limit
  its header now states), so that operation is not issued then.
- Every fuzz target has a `fuzz.py` entry naming its corpus and executable, and no entry is stale; a test enforces it
  (the bundle target had none). The replay driver is headless, so an engine assertion ends a replay with its
  evidence instead of a modal dialog that waited out the CTest timeout.
- Not yet: capture, policy, flight-record and run-record readers; command decoding; resource reload; the compiled
  and reference CEIR comparison; the reinvestigation of the unknown-origin CEIR and CKIR artifacts.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-10a-evidence-readers.md)):
- The crd-perf evidence readers are three more fuzz targets on the same harness, each with its corpus under
  `tests/foundation/perf/fuzz/corpus/<reader>/` replayed on every lane: `crd-fuzz-perf-flight`
  (`decode_flight_record`), `crd-fuzz-perf-assets` (`parse_flight_policy_asset`, `parse_fault_plan_asset`,
  `parse_fault_plan` and crd-toml beneath them) and `crd-fuzz-perf-capture` (`validate_capture_buffer` and
  `CaptureView`). The bundle corpus moved to `corpus/bundle/`. Each oracle is what the writer can produce, never the
  reader's own verdict, and each seed function checks every seed's verdict as it writes it, so a regenerated corpus
  cannot carry an honest seed the reader now refuses or a forged one it now accepts.
- A flight record is evidence only if a recorder could have written it. `decode_flight_record` checked bounds and
  order but accepted records that contradict themselves; it now also refuses a trigger kind or suppressed kind the
  recorded policy does not arm, a suppression count without kinds (or kinds without a count), an end reason the post
  window contradicts (post-events-full exactly when `post_events` landed, elapsed only under a time bound, shutdown
  and fatal ends exactly when `post_truncated` says so), a capture without the ring that is not fatal or holds events
  or a drop count, events that are not one contiguous run across the trigger, a clip flag on a message that does not
  fill its buffer, an event below Warning under a Basic policy, and an unavailable replay without its reason. An
  accepted record re-encodes to exactly its own bytes. No record version changed: every capture a recorder writes
  still reads.
- The CPROF reader validated the header's section sizes and then trusted everything they point at; the profiler panel,
  `cprof-export`, `capture.validate` and the Perfetto export all read through it. `validate_capture_buffer` now
  refuses: a name-blob size that wraps the section sum (the frame records then sat before the buffer); a thread's
  sample array outside the buffer, before the frame records or misaligned; a name blob smaller than its 8-byte header
  or not a multiple of 8 (the frame records after it misaligned), whose slot count disagrees with the header or whose
  slots and strings overrun it, with an offset past its strings, or whose strings do not end with a terminator;
  unterminated thread, counter and allocator names; an unknown counter kind or type; more threads, counters or
  allocators than the profiler's tables, and frame records whose counts exceed their arrays (the export indexed a
  record's values by the header's counter count); a buffer that is not 8-aligned. `CaptureView` keeps trusting a
  validated buffer. No on-disk layout or version changed; every capture the writer produces still validates.
- crd-toml, which reads every authored asset and configuration, had no bound on nesting: a 20,000-deep array or
  inline table overflowed the stack (0xC00000FD on win-debug). It now refuses nesting deeper than 64 levels (tables
  from header paths and array-of-tables elements, arrays and inline tables together), an integer outside the 64-bit
  signed range (read as `INT64_MAX` before, so `seed = 9223372036854775808` armed seed 9223372036854775807), a float
  that overflows a double, and a number token longer than 63 characters (read as its first 63 before). No committed
  asset or configuration is affected.
- libFuzzer found the Perfetto export computing a span as `end - begin` in `i64`, which overflows for a hostile
  capture; a duration is now the difference without overflow (0 for a reversed span, at most `INT64_MAX`), and the
  minimized input is adopted. The policy readers had no defect of their own: a refusal leaves the output untouched,
  and every accepted policy and plan writes a canonical document or text that reads back to the same value.
- Fuzzing rounds for DIAG.10a run single-process. Under four concurrent processes, two perf targets (the unchanged
  bundle target among them) showed input-independent register and stack corruption that no artifact reproduces and
  single-process runs never showed; the cause is not established and stays an open item of the row, not an
  explanation. (2026-10-11: concurrency is not a condition of it. It has since appeared in single libFuzzer
  processes, in the replay driver and in an unsanitized gcc build; see the container-overflow control block below.)
- Not yet: run records (`decode_record`), command decoding, resource reload, the compiled and reference CEIR
  comparison, the virtual-memory and streaming allocators, and the unknown-origin artifacts.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-10a-run-records.md)):
- Run records are the fuzz target `crd-fuzz-ceir-record` (`tests/execution/ceir-cook/fuzz`, corpus replayed on every
  lane, libFuzzer on `linux-clang-fuzz`). Each input is decoded as given and again with its header's payload size and
  checksum recomputed: the checksum guards integrity, so without the reseal a mutated payload never reaches the
  payload reader. The program blob is carried, not read; the CRDR reader beneath it is `crd-fuzz-ceir-binary`'s.
- A run record is evidence only if a recorder could have written it (the DIAG.10a flight-record rule).
  `decode_record` checked bounds, each executor's own fields and the trace and read-stream counts; it now also
  refuses what `replay.record`, `record_host_run` and the device recorder never write: an input state its need
  contradicts (not-needed exactly when no op needs it), program, build or entry arguments that are not needed-and-
  recorded, external results recorded, a schedule recorded by an executor other than the host (or missing on a host
  record that needs it), a device tolerance recorded off the device executor (or missing on a device record that needs
  it), a seam input (random, clock, host-state) recorded when only possibly needed or when the record lost reads past
  its 65,536-read bound, a kept read of an input the record says is not needed, a cancelled plan or host run, a device
  kernel whose text does not hash to its recorded hash, and device kernels or buffers in a plan or host record.
  `replay.prepare` presented a record's recorded states as available inputs, so these claims reached its answer.
- Every count is checked against the bytes that remain before an array is reserved for it: a record under 500 bytes
  could make the decoder reserve 3 MB of events, 1.5 MB of reads or 16 MB of device words before it found the bytes
  missing. The target bounds the decoder's allocations to eight times the input plus 64 KiB.
- Oracles beside the recorders' restated guarantees: the same verdict twice, an accepted record re-encoding to its own
  bytes, its missing inputs exactly the ones its states name (external results unless not needed), and no divergence
  from a trace that repeats it.
- Record files that make such claims are now refused at `replay.run` and `replay.prepare` as not decoding, before
  `record_missing_inputs` sees them; it remains the guard for in-memory records (the host and device replays take
  them), and the DIAG.9b cases check both routes. No record layout or schema changed; every record a recorder writes
  still reads.
- Not yet: command decoding, resource reload, the compiled and reference CEIR comparison, the virtual-memory and
  streaming allocators, the unknown-origin artifacts and the concurrent-load corruption.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-10a-command-decoding.md)):
- Command decoding is the fuzz target `crd-fuzz-ceridc-diag` (`tests/tools/ceridc/fuzz`, corpus replayed on every lane,
  libFuzzer on `linux-clang-fuzz`; the ceridc module owns `tests/support/fuzz`). Its ingress is the line ceridc's
  stdio MCP transport reads: the owned JSON parser (crd-asset-io), `mcp_handle`, the diag tool's argument decoding and
  `DiagCommandService::execute` with every command ceridc binds plus the sandbox's flight and fault commands, so every
  registered argument check is reachable. An input's host line chooses the grant and whether there is a file root;
  the root is a directory that does not exist, so a path command reaches its argument check and handler and opens
  nothing. A line that could reach one of ceridc's file verbs (import, cook, query, instantiate, sequence, render,
  export) is not fed, because those read and write paths the input names; their argument decoding is covered in
  crd-ceridc-tests.
- Oracles, none of them the decoder's own verdict: an independent RFC 8259 validator (iterative, decoding UTF-8 to
  code points) must agree with the parser within its documented limits (64 levels, numbers a double holds); every
  reply is one line of valid UTF-8 JSON that follows the JSON-RPC rules (-32700, -32600 with a null id, no reply to a
  notification, the request's id echoed exactly); the diag tool's documented decoding is restated and the report it
  returns equals the native answer for the page the report names, byte for byte; the service's refusal order is
  restated (a refusal runs no handler, an argument check answers only bad-argument or unavailable); every document's
  fields equal the typed result, its pages respect their item and byte bounds, a walk to the end covers the snapshot
  once with each cursor repeatable, and the run returns every byte it allocated.
- Findings fixed. The JSON parser (glTF, OTIO and every MCP line): numbers went through `strtod` on a 63-byte copy,
  so `+1`, `01`, `.5` and `1.` were accepted, a longer token was read as its first 63 characters, and an overflow
  became infinity; a number now matches the RFC grammar and is read whole by `std::from_chars`, one a double cannot
  hold is refused. Raw string bytes were never checked, so ill-formed UTF-8 reached every consumer; it is refused
  (`crd/core/utf8.hpp`). `as_i64` cast any double, undefined past the i64 range (an MCP id of `1e300`); it answers the
  default there. The MCP transport answered a string id as 0, truncated a fractional one and rounded one past 2^53;
  it silently dropped a batch array or a bare value (a client waits for a reply that never comes); it compared method
  and tool names as C strings cut at a NUL (`"ping\u0000x"` was `ping`, `"diag\u0000"` was `diag`); it cut a string
  argument past 511 bytes, or at a NUL, to another path; it read a `dry_run` of another type as false (the string
  `"true"` wrote the file); and it cast frame counts and translations from any double. Ids now echo exactly (a string,
  or an integer within 2^53 - 1), anything else is -32600, names compare whole, and an argument is used as sent or the
  call is -32602. Both JSON writers (crd-perf's and crd-asset-io's) passed every byte at or above 0x80 through, so a
  request's raw bytes (a CLI argument, an argument a check echoes in its reason) or evidence bytes (a bundle's module
  names, an OBJ's names in an import report) made a document that is not UTF-8, and the service cut the command echo
  at 64 bytes inside a sequence; ill-formed bytes are now written as `\ufffd` and the echo stops at a boundary.
- libFuzzer found that a refused request's typed `DiagResult` kept `cursor` at 0 while its document named the
  request's cursor; `refuse()` now sets every typed field the document states. A refusal echoes the request's cursor,
  which a native caller can set past 2^53: only the cursors the service issues are promised exact in a double.
- Duplicate member names keep the parser's first occurrence (RFC 8259 leaves them to the implementation); the
  transport, the service and the target read them the same way.
- The commands' file readers (`bundle.inspect`, crd-ceir-cook's bounded file reader) open files directly, outside
  crd-core's I/O fault seam, so neither fault injection nor this target observes which paths they open; the target's
  root does not exist, so they open nothing. The diag asset reader consults the seam since 2026-10-25 (DIAG.12).
- Not yet: resource reload, the compiled and reference CEIR comparison, the virtual-memory and streaming allocators,
  the unknown-origin artifacts and the concurrent-load corruption.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-10a-resource-reload.md)):
- Resource reload is the fuzz target `crd-fuzz-resources-reload` (`tests/assets/resources/fuzz`, corpus replayed on
  every lane, libFuzzer on `linux-clang-fuzz`; the resources module owns `tests/support/fuzz`). Its ingress is a pack
  file on disk handed to the real `ResourceManager` (the production entry points take paths): an even first byte makes
  the rest one pack file, read by `crdr_read` directly and then mounted, loaded, reloaded, evicted, loaded again and
  unmounted; an odd one makes it a script over two pack files and one manager (packs built through `CrdrWriter` and
  then overridden with a lying range, a duplicated id, a torn tail, a compressed or dropped chunk; raw bytes; in-place
  corruption; mount, unmount, `load_sync`, release, `reload_mount_now`, a drain to a zero budget, pin and unpin).
  `poll_hot_reload` is not driven: its mtime has one-second resolution, and both reload paths share `do_reload_mount`.
- Oracles, none of them the manager's own verdict: an independent restatement of the CRDR layout decides acceptance,
  places every uncompressed payload at its exact offset and every compressed one at exactly what zstd decodes;
  `crdr_read` allocates at most eight times its input plus 64 KiB plus what its accepted claims decode to; a model of
  the manager holds the live table, every loaded block (state, generation, bytes, resident size), pins and the
  subscribers' events, and after every operation `find_entry`, `entry_count`, `mount_count`, `current_memory_use`
  and every held handle must equal it; a payload only ever reaches its own type's loader; the run returns every byte.
- The contract this pins, now stated in `resource_manager.hpp` (the v1f description was silent on all but the first):
  a pack is taken whole or not at all by mount and by both reload paths: refused when it cannot be read or parsed,
  names bytes past its end (torn) or names one id twice; the live table serves each id from the newest mount whose
  manifest names it, so unmount and a reload that drops an id fall back to an older mount; a reload makes the pack the
  mount's manifest (ids it adds are served, ids it drops leave or fall back) and swaps only the loaded payloads of
  their own type that the mount serves; an id keeps its type, so neither a reload nor a re-issue after eviction ever
  hands a payload to another type's loader or a handle; a reload re-accounts the resident size against the budget.
- Findings fixed. `crdr_read` sized its decompression buffer by the chunks' claims before reading a byte of them: a
  97-byte file claiming 1 TiB reserved it, and two claims whose sum wraps reserved 16 bytes and decoded 32 into them
  (a heap overrun); a claim is now bounded by what its stored bytes can decode to (3-byte zstd block headers, 128 KiB
  per block, `crdr_max_decompressed_size`), and a frame that decodes to fewer bytes than claimed is refused instead
  of leaving a zero-filled tail. `do_reload_mount` copied a new manifest's offsets into the live table before any range
  check (a torn pack picked up by hot reload left entries pointing past the file) and checked a reloaded range as
  `offset + size > file`, which wraps (an offset near 2^64 read before the buffer); it now refuses what mount refuses,
  before touching anything. It reloaded an id a newer mount served from the older pack's bytes, swapped a retyped
  entry's payload into a block of the old type (and freed the old payload with the new type's loader), ignored ids a
  reload added, kept dropped ids pointing at stale offsets in the new file, swapped a duplicated id twice, and never
  re-accounted the resident size. Unmount erased an id an older mount still provided, contrary to its header. A
  re-issue after eviction loaded a retyped entry with the new type's loader into a handle of the old type. A manifest
  naming one id twice mounted, the later entry silently winning.
- The replay driver ends the process on a Debug CRT report (a heap overrun found at a free), which until now opened a
  modal dialog that held a replay until the CTest timeout.

Settled (2026-10-10, [session](../sessions/2026-10-10-diag-10a-ceir-differential.md)):
- The compiled and reference CEIR comparison is the fuzz target `crd-fuzz-ceir-differential`
  (`tests/execution/ceir/fuzz`, corpus replayed on every lane, libFuzzer on `linux-clang-fuzz`). An input decodes to a program, never to a parse
  failure: a header (functions, the step budget, a host input script or none, the entry arguments) and each function's
  body as a stream of op choices over arith, core control flow, the three cell kinds, `func.call` with recursion,
  async and task tokens, data-parallel bodies and the input ops, kept inside the module-validity set a cook enforces.
- Oracles: the reference interpreter and the compiled plan agree on the error class (the ExecError and RunError pairs
  `plan.hpp` names), the values, every cell, every map output on success and the op an error blames (by stable id);
  a reference error with no compiled counterpart is a finding, and the only accepted compile refusal is the documented
  one (a parallel body holding a cell or reading host input, refused whole at compile and by the reference when the op
  runs). The plan run twice, a fresh reference session and the printed and serialized twins give the same outcome.
- What a step is, for both tiers: a dispatched op that is not a terminator, a loop iteration, a parallel-range index
  and a fold step; a parallel body's steps are the run's, so one budget bounds the whole run. The reference charged every `core.yield` and `func.return` a step, so the tiers ran out at
  different ops (inside a `core.while` condition one step per iteration); it no longer does. `plan::run` takes the
  budget (`max_steps`, the reference's default `plan::kDefaultMaxSteps`), so FuelExhausted compares at a small budget.
- The nesting bound `exec::kMaxNesting` (64): an op with a region or a `func.call` counts one level while it runs, and
  an op that would nest past the bound fails `NestingTooDeep` (appended to ExecError and RunError, `kLastExecError` and
  `kLastRunError` bounding the run-record decoder) before it runs, blamed on that op in both tiers. A sub-interpreter
  running a parallel body starts at its parent's level. Unbounded recursion used to overflow the native stack: on
  win-asan (1 MiB main-thread stack) after about 190 levels, and after about 180 through a `map_reduce` body; the
  bound is a third of that and equals the loaders' static region-nesting cap. It is a resource limit like the budget.
- A parallel range's count and indices use unsigned arithmetic (`exec::parallel_range_count`, `parallel_range_index`):
  `hi - lo` overflowed i64 (undefined), wrapping `[INT64_MIN, INT64_MAX)` by 2^62 to an empty map. Each index is one
  step of the body budget in both tiers, and the reference's map grows per index: it used to reserve a slot for every
  index before running any body, so a 2^31-index range reserved 16 GiB. A `for` loop stops at the last iteration
  within `step` of `hi` instead of computing `iv += step` past INT64_MAX (undefined; on a wrapping target the loop ran
  on until the budget ran out).
- A compiled `parallel_for` or `map_reduce` re-entered from its own body (a body call recursing into it) cleared and
  appended to the op's one map slot, so the inner activation's entries landed in the outer map ([0, 1] where the
  reference reports [1]); each activation now builds its own map and stores it when it completes, as the reference
  does, reusing the slot's storage.
- Found by libFuzzer: a parallel op inside another parallel body stored its map in the reference's parallel
  sub-interpreter, which was then discarded, so the reference reported no map where the plan reported the last
  completed activation; the parent now adopts a sub's map outputs when the body's range completes
  (`Interpreter::adopt_map_outputs`). And the plan opened a parallel body's fresh token store by moving the whole
  run-wide store out and back, quadratic in a loop that both launches tokens and runs a parallel op (an 8,000-iteration
  loop took 7.7 s on win-debug); a body's store is now the tail past `token_base`, dropped when the body ends. And
  both tiers gave a parallel body a copy of the remaining budget and restored the parent's afterwards, so a body that
  re-enters the program (a 31-byte input: `f0(n)` mapping over `[-63, n)` and calling `f0` from the body) ran for
  minutes, its work growing as the budget to the power of the nesting depth; the sequential tiers now charge the
  parent what its bodies spend (`Interpreter::charge_from`). The parallel host provider keeps per-item budgets.
- Not yet: the host provider (`crd-ceir-host`, the parallel tier) still computes its range count the old way and
  pre-sizes per-index arrays from it, its worker sub-interpreters start at nesting level 0, and each item has its own
  budget. The last libFuzzer round ended in an ASan SEGV with a zero frame pointer and no artifact, on this target
  that does not link crd-perf, single-process; the 588 inputs of that round replay clean (the concurrent-load
  corruption item, still open).

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10a-host-parallel-range.md)):
- The crd-jobs host provider (`crd-ceir-host`, the parallel tier) counts a parallel range with
  `exec::parallel_range_count` and indexes it with `parallel_range_index`, as both sequential tiers do. It computed
  `hi - lo` in i64 (undefined past half the line; `[INT64_MIN, INT64_MAX)` by 2^62 came out empty) and cut the count
  to 32 bits (2^32 + 3 indices ran as 3, 2^33 as none).
- The parallel tier's budget rule. Everything the submitting thread runs spends the run's budget (the provider's
  submitting interpreter, 2^24), as in the reference: its own ops, one step per parallel index and the map_reduce fold
  (one step per fold step plus the fold's ops, charged back with `charge_from`). The indices are spent at once
  (`Interpreter::spend_fuel(steps)`) before anything is sized from the count, so a range past the budget is
  FuelExhausted on the parallel op before a map slot, an error slot or a job exists; the provider used to size three
  per-index arrays from whatever count the program chose and never charged the indices. Each body a pool worker runs
  (a range index's body, a pooled launch body) keeps its own budget `sub_fuel`, so a body's budget does not depend on
  the job split. With bodies that dispatch nothing, the submitting interpreter ends with exactly the reference's
  remaining budget. The fold used to run on one `sub_fuel` budget, uncharged.
- A body the provider runs on a sub-interpreter (a range index on a worker, a fold step, a pooled launch body) starts at
  the level of the op that runs it, so it stops at `exec::kMaxNesting` on the op the reference blames. The level is read
  on the submitting thread while the op runs and travels with the job (`Interpreter::set_nesting`), because a pooled
  launch's body runs after its submitter has moved on. Each body started at level 0, so a body run 20 levels down had
  20 more levels than its program.
- Bodies on pool workers run on Medium fibers (512 KiB). On Small (64 KiB) fibers a win-asan worker faulted on the guard
  page past about 35 levels (34 fitted, 36 did not; about 1.9 KB a level), so a parallel body recursing 30 calls deep
  ended the process with an access violation and no report. 64 levels take about 120 KiB. A body never waits, so it
  holds a fiber only while it runs, one per worker; Large has 16 fibers, fewer than a pool may have workers. This
  assumes a pool has at least as many Medium fibers as workers: every caller that runs the provider (crd-sandbox and
  the application through the default `jobs::Config`, ceridc, the tests) keeps the default 64, and only the
  fiber-exhaustion specimen lowers it.
- Each body's scratch allocator starts with a 64 KiB chunk and grows by chunks: every range index, pooled launch and
  fold allocated the default 256 MiB first chunk, which the debug heap fills (256 indices took 16.8 s on win-debug,
  0.27 s now) and which each busy worker held.
- Unchanged and recorded: a parallel op inside a body the provider runs on a sub-interpreter (a range body, a fold, a
  pooled launch body) fails NoSemantics, because sub-interpreters carry no parallel context, where the reference runs
  it; and a pooled launch's error reaches the run at its await, where the reference raises it when the launch runs.
- Not yet: the provider is not in a fuzz target. Per-item budgets, the nested-parallel refusal and asynchronous launch
  errors make its agreement with the sequential tiers a filtered oracle (the same answer when the reference completes,
  no body needs more than `sub_fuel` and no parallel op runs inside a body), not an identity.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10a-vm-streaming-allocators.md)):
- The `VirtualMemoryAllocator` and the `StreamingAllocator` are two more kinds of `crd-fuzz-memory-allocators`
  (appended to its kind enum; every committed input's kind byte is below the old count, so each keeps its kind). Each
  input reserves at most 16 MiB of address space, commits only what it bumps through and releases the range with the
  allocator, so an input costs a few system calls and no lasting address space.
- The arena is held to an exact model: the bump offset (the bump arenas' model), the committed high-water mark (the
  next multiple of the commit block, capped at the reservation; a purge back to the page holding the live top), the
  most recent allocation (reallocated in place, everything else moved), freshly committed bytes reading zero, the
  committed tail poisoned under ASan, `owns()` bounded by the live top and `bytes_in_use` equal to the offset. Its
  `reallocate` runs only where the model shows neither fatal path (past the reservation, the copy's `allocate`) can be
  reached; a `vm::commit` failure is not modelled.
- Defects fixed in the arena: `try_allocate` asserted on a non-power-of-two alignment instead of refusing it (the
  DIAG.3a contract every other allocator keeps); a commit block that is a page multiple but not a power of two (12 KiB)
  was rounded with the power-of-two mask, committing page multiples that are not block multiples; a commit block too
  large for any page multiple wrapped to 0 and became one page (it now commits the whole reservation); a reallocate of
  the top block asking for a stricter alignment than the block has returned it in place, misaligned (it now moves).
- The streaming allocator runs under a residency policy the input scripts: shed blocks of the category, of any
  category, one block, nothing, or claim bytes it never freed. Every category's `used()` moves by exactly what is
  allocated and what release un-charges, staging claims and resident blocks never overlap, a lying policy ends in a
  refusal after exactly 17 consultations (the retry bound) and a request nothing can serve never consults the policy.
- Defects fixed in the streaming layer and the heap under it: a request no resident heap state can serve (a
  non-power-of-two alignment, a size past the whole reservation, an alignment past any chunk) reached the eviction
  loop, so the policy shed resident data for it; `GrowableTlsfAllocator::try_allocate` walked its chunks with a
  non-power-of-two alignment, every chunk refused it, and it then took a new chunk from its parent and refused again,
  so each such request left a chunk behind (committed and kept for good under a virtual-memory parent; up to 17 per
  streaming request); the budget compared the request but charged the heap's block, so rounding carried a category
  past its budget (999 bytes against a budget of 1000 charged 1008). The budget now holds the charge: a block rounded
  past the remaining budget goes back to the heap and the request takes the eviction path, and one whose block alone
  exceeds the whole budget is refused without consulting the policy. The charge exceeds the request by less than
  64 bytes (16-byte granules and a remainder too small to be a free block), which the target checks on every success.
- Not yet: the reinvestigation of the unknown-origin CEIR and CKIR artifacts, and the concurrent-load corruption.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10a-unknown-origin-artifacts.md)):
- The four loader targets carry cross-format oracles, the identities their headers already state: an accepted CEIR
  text module serializes to a blob that deserializes and prints to the same text (`binary.hpp`: print . deserialize .
  serialize == print); an accepted CEIR blob prints text that the parser accepts and prints identically, so every
  module the binary loader accepts has a text form; an accepted CKIR blob writes `.ckir` text that reads back to the
  same blob (`ckir_asset.hpp`). The CKIR text target already compared the two forms. This is the strengthened harness
  the acceptance names for the retained artifacts.
- The six round-four artifacts REPO.DEV.11 adopted with unknown origin each replay clean on that harness: alone, 10,000
  times in one libFuzzer process under ASan and UBSan (stack-use-after-return, strict string and init-order checks),
  and under valgrind memcheck on the unsanitized linux-gcc-debug build (with the whole of the four corpora). None
  reproduces its recorded symptom, so they stay labelled unknown; `183a3e2e` is now refused by the name rule below
  (its bytes hold an op name the text form cannot express), which is a property of its bytes, not an explanation of
  its round-four report.
- A name the CEIR printer writes bare (op names; struct, enum, param and trait names; field and case labels; a
  symbolic dim; a type- or attr-class name) must be `[A-Za-z_][A-Za-z0-9_.]*` in a blob, the text lexer's rule; a
  symbol ref must not be empty and a type attribute must name a type. Each of these loaded before and then printed
  text the parser refused, or, for the type attribute, asserted in `type_of` while printing (an out-of-range read
  where asserts are off).
- The CEIR text reader accepts `!tuple<>` and `!variant<>`: the type model, the factories, the binary form and the
  printer all allow zero members, so the reader was the narrower side.
- A CKIR blob must hold only what the builder and the text reader produce (`records_canonical` in
  `ckir_serialize.hpp`): every enum byte names an enumerator, a shape's rank is within [0, 8] with every dim past it
  zero, and every stage-output slot past `n_out` is the default one. An out-of-range enum indexed past the text
  writer's name tables; a dim past the rank or a stray output slot loaded and then lost in the text form, so two blobs
  of one program hashed apart. The `.ckir` reader reads INT64_MIN, which its writer emits for a shape dim.
- The corruption of unknown origin is not settled. It recurred in single libFuzzer processes on targets that do not
  link crd-perf, never from a reproducing input, and each site this time was in `crd::containers::Array`'s
  container-annotation path (`annotate_size`, `asan_annotate_live_range`, a byte `push_back` right after its
  annotation). A known-answer CPU probe found no wrong result, and the host's own toolchain processes crash at
  varying offsets. The discriminating local experiment is a timed comparison of the same rounds with
  `ASAN_OPTIONS=detect_container_overflow=0` against the default; a run on another machine is the user's.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10a-container-overflow-control.md)):
- The container-annotation path is not the cause of the corruption of unknown origin. With
  `detect_container_overflow=0` (shown to disable the annotation in this runtime: the Array live-range case fails
  under it) libFuzzer processes paired by seed, run count and moment faulted 7 times in 12 against 8 in 12 with the
  default, about one event per 1.5 M and 1.0 M executions, and one arm-B fault was inside
  `asan_annotate_live_range` itself. That path is where corruption is noticed, not where it is made.
- Neither libFuzzer, the sanitizers nor the input is needed. The replay driver over 790 inputs faulted 130 times in
  about 2.31 M executions; an unsanitized linux-gcc-debug loop faulted twice in about 4.68 M, once as a wrong result
  (serialize output its own deserializer rejected) on an input it handled correctly 5,922 other times; every one of
  the control's 13 artifacts replays clean and one is a committed seed. `MallocAllocator::try_allocate` asserted
  `is_pow2(alignment)` on the constant 8 its caller loads immediately before the call, a value that only the CPU,
  the kernel or hypervisor's register save and restore, or a foreign write into the live stack can change.
- No defect in the engine, the harness or the instruments is indicated. Which part of the execution environment
  misbehaves (the host CPU or the WSL2 guest) is not separated here: native Windows loops were clean at small
  exposure with different executables, a compile-determinism probe (716 concurrent compiles) and the known-answer
  probe were clean, and the rate rose about twentyfold within the session. The separating run is the user's (the
  same loops on another machine, or on this one booted into native Linux).
- Artifacts this host's fault produces are counted and recorded, not adopted, until that run (a dated exception to
  the adoption rule in [test instruments](test-instruments.md)).

<a id="diag-10b"></a>
## DIAG.10b — coverage and diagnostic effectiveness

Measure branch/region/error-path and state-transition coverage for the DIAG scope with the instrumented denominator
explicit. Include mutant/seeded faults that must be detected; do not chase a percentage while missing key error paths.
Track coverage exclusions and flaky/nonreproduced cases as owned evidence. A quarantined test cannot close a gate.

Acceptance: broken sanitizer activation, suppressed reports, removed guard, lost fatal record and incorrect symbol
mapping make the diagnostic suite fail. Read JSON/JUnit and actual artifact contents; exit status alone is insufficient.
No implementation-mirroring unit tests substitute for these observable failure cases.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10b-seeded-detector-failures.md)):
- The five acceptance classes are seeds in [seeded-faults.json](../../tests/support/diag/seeded-faults.json), run by
  [diag-seeded-faults.py](../../scripts/diag-seeded-faults.py) against a real build lane: control run, seed, rebuild,
  run, byte-exact restore, rebuild, run. The verdict is read from CTest's JUnit and each failing test's output (the
  registry names the diagnostic text it must print); a passing test is SURVIVED, a failure without that text is
  WRONG_FAILURE, a mutant that does not compile is never a detection, and nothing is rerun. Eleven seeds: specimens
  and tests built without the lane's sanitizer, ASan and TSan reports redirected to a log file, a TSan and an LSan
  suppression naming Cerid's own specimens, MSVC ASan's `continue_on_error`, the rear-redzone check removed, a
  truncated Windows minidump and Linux crash record, and half-compared symbol identities.
- A lane's sanitizer promise comes from its configuration (`crd::diag::lane_promise()`, compiled from
  `CRD_ENABLE_ASAN`/`CRD_ENABLE_TSAN` and the toolchain), not from a binary's own flags. Before, the sanitizer
  specimen tests judged a specimen by its own tag or by the test binary's flags, so a specimen, or a whole lane, that
  lost `-fsanitize` reported InstrumentAbsent and passed: the activation seeds survived on linux-gcc-asan (the
  use-after-return and leak cases) and linux-clang-tsan (the data-race case), and the whole-lane seed survived every
  case there. The tests now also require their own binary to carry the promised instrument.
- A sanitizer catch needs the sanitizer's report. The specimen harness reads the child's stderr with its stdout
  (and repeats it to the test log) and returns SanitizerCaught only when the output holds `ERROR: AddressSanitizer`,
  `ERROR: LeakSanitizer` or `WARNING: ThreadSanitizer`; a death without it is Unexpected, with the reason
  `exit <code> without an AddressSanitizer report in the child's output` (or `a ThreadSanitizer`), which the cases
  print and the log-file seeds name. Before, any non-zero exit of a sanitizer-tagged specimen counted, and both
  log-file seeds survived on every sanitizer lane.
- The committed suppression files may name only third-party shared libraries; the verdict tests refuse any other
  entry. CI has no retry: no `--repeat` or `--rerun-failed` in [ci.yml](../../.github/workflows/ci.yml) or the CTest
  wrappers, and the registered-failure gate admits only reproduced third-party defects.
- Not yet measured (2026-10-11, superseded by the next block for coverage and the register): branch, region,
  error-path and state-transition coverage of the DIAG scope with its denominator, the register of coverage
  exclusions and nonreproduced failures, and the seeds on hosted lanes.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10b-coverage-and-failure-register.md)):
- The DIAG scope is a committed denominator, [coverage-scope.json](../../tests/support/diag/coverage-scope.json):
  63 files in nine groups, each naming its rows (core assertion, crash and real-time capture; fault seams;
  minimization; the memory module; the job observer, interleaving and hang code; all of crd-perf's sources; the
  CEIR inspection and command-decoding sources with ceridc's diag and MCP transport; run records and replay; the
  specimen harness). Ordinary logging, the application host (DIAG.11a) and product algorithms are outside it.
- The route is clang source-based coverage on the `linux-clang-coverage` preset (`CRD_ENABLE_COVERAGE`; diagnostic
  tier, never hosted). MSVC has no equivalent, so a Win32-only branch is outside this lane's denominator, and a
  value produced only there is owned `windows-only`; such an exclusion is stale the moment the site is compiled on
  the lane. [diag-coverage.py](../../scripts/diag-coverage.py) runs the test set once (eight CTest programs, seven
  corpus replays and the schedcheck program, a CTest program since DIAG.11b), requires every selected test to pass
  in the JUnit, merges profiles with `--failure-mode=any` and exports exactly the scope files from every program,
  specimen and ceridc.
- Error-path coverage is defined per outcome: every value of each registered result/status enum must be produced at
  a site whose innermost coverage region ran (a producing site is `Enum::Value` that is not a `case` label or an
  operand of `==`/`!=`). State-transition coverage is per transition site (the flight recorder's window machine:
  every `make_word` or `word.fetch_or` of the recording word naming a state), not per edge pair. Region, branch, function and
  line totals are reported per file and group and are never a threshold.
- Gaps are owned evidence, never silently missing: each unrun value or site needs an exclusion with a class and a
  reason, and an exclusion that matches nothing, now runs, or contradicts its class is stale. A child that dies
  writes no profile, so the Linux crash record's write path is `dying-child`. The first measurement found 30 gaps;
  15 are real untested error paths owned as `owned-gap` with their rows (four record-minimization verdicts, the
  host-executor adapter's refusals, inspection script and host-load failures, a failed record write, a cancelled
  external-results analysis, the flight recorder's fatal path with the ring held, the harness's own timeout
  verdict and two generation wraps with no seam), and one declared verdict (`MissingSymbolizer`) was never produced
  (since DIAG.12, 2026-10-25, the harness returns it for a report with no symbolized frame).
- [nonreproduced-failures.json](../../tests/support/diag/nonreproduced-failures.json) registers flaky and
  nonreproduced failures with signature, reruns, status and session; a test fails if any build file, CI step or
  script reads it, so it cannot become a quarantine list.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10b-owned-gaps-closed.md)):
- Twelve of the fifteen owned gaps are closed by `[diag10b]` cases that assert the answer, not only the line: the
  record minimizer's Unrunnable (a cut blob, an undefined entry), NotReproduced (inputs failing elsewhere or not at
  all), Budget (the record made under the bound still fails the recorded way) and Cancelled (with
  `check_external_replay`'s own Cancelled); the host executor adapter's Unavailable and BadArgument when a host calls
  it directly (the service checks those first); a record written where no file can be created; an inspect-host load
  of asset 0 or of another asset; a script on a host with nothing loaded; `program.inspect` past its wait bound; and
  the specimen harness's own Timeout. Their exclusions are removed: the measurement with the old registry was STALE
  naming exactly those twelve, and PASS without them (regions 9,210/10,356, branches 5,967/7,634, lines
  17,107/19,662). Each producing site was broken once and its case failed.
- The three left need a seam in an engine header: a generation setter for `DiagnosticPolicyStore` and for
  `FlightRecorder` (2^32 - 1 loads otherwise), and a way to hold the flight recorder's ring mutex across
  `finish(Fatal)` (no public call holds it while calling out). A host lacking a program's dialects reads it with every
  op opaque, so the minimizer refuses it as needing external results rather than calling it unrunnable.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10b-engine-seams.md)):
- The last three owned gaps are closed through test-only seams, compiled only where `CRD_ENABLE_ASSERTS` is (never
  in a shipping build, following `crd::crash::test_fatal_path`): `DiagnosticPolicyStore::test_set_generation` and
  `FlightRecorder::test_set_policy_generation` move only the counter (0 and a recorder before init are refused), and
  `FlightRecorder::test_with_ring_held` calls a function while its thread holds the ring's collection lock (the ring
  mutex until 2026-10-24). A seam never
  returns a status itself, so the producing sites the measurement names are the engine's own.
- `[diag10b]` cases: a policy load and a flight reload one past generation 2^32 - 1 are refused with
  RejectedGenerationWrap, the active policy, the generation and the flight ring stay (the six events recorded before
  the refusal are the next capture's pre window), and validation still refuses a malformed candidate first. A
  crd-jobs worker holds the ring while the test's thread runs `finish(Fatal)`: with nothing pending nothing is
  delivered and the recorder stays armed; with a trigger pending the capture carries the trigger alone
  (`ring_unavailable`, `post_truncated`, no events, the provisional sequence, the replay recipe), the recorder enters
  FatalEntered (nothing is recorded, triggered or delivered again), and the capture round-trips through the record
  encoding. Seven breaks (both wrap guards, the generation seam, the seam not holding the ring, and the fatal
  branch delivering nothing, re-arming or not saying so) each failed a case.
- The measurement with the old registry was STALE naming exactly the three, and PASS without them (853 CTests;
  regions 9,235/10,372, branches 5,989/7,644, lines 17,163/19,691; the seams add 16 regions to the denominator).
  No `owned-gap` exclusion is left: the 20 remaining are `windows-only`, `dying-child`, `default-value`,
  `resource-failure`, `other-configuration` and the one `no-producer` verdict.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10b-seeds-on-hosted-lanes.md)):
- The seeds run hosted on the five lanes they were qualified on (win-debug, win-asan, linux-gcc-debug, linux-gcc-asan,
  linux-clang-tsan), as the `Seeded detector failures` step after each lane's build and tests pass, in that lane's
  own tier, with the test preset's environment as every run's base. The runner's exit is the step's verdict (0 only
  when every applicable seed is DETECTED; a lane where none applies is ZERO_RUN), nothing is rerun, and the report
  with every phase's build log, CTest log and JUnit is part of the lane's evidence artifact. The seeds' CTest runs
  share the lane's build tree, so the runner restores the lane's `Testing/Temporary/Last*.log` afterwards: the
  uploaded record stays the lane's own run, never a `LastTestsFailed.log` naming the tests a seed broke.
- The step is a multi-preset `preset_steps` entry of the [tier mapping](ci-tiers.md); `check-ci-tiers.py` requires
  it in each listed preset's job, gated on exactly those presets. The verdict tests derive each hosted lane's facts
  from its configure preset, so a seed no hosted lane runs or a hosted lane no seed applies to fails them.

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

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10c-numerical-mode.md)):
- The declared numerical mode is the thread-start mode: MXCSR `0x1F80` (every exception masked, round to nearest
  even, no flush-to-zero, no denormals-are-zero) on both platforms, and the x87 control word `0x027F` on Windows
  (the CRT's 53-bit precision) and `0x037F` on Linux (glibc's 64-bit precision). SSE/AVX float and double
  arithmetic is IEEE binary32/binary64 under it; x87 arithmetic is Linux's `long double` only, since MSVC compiles
  none on x64. Only control state is a mode: the sticky exception flags are results of past operations and are never
  compared. No algorithm or unit changes; code that wants another mode sets it for a bounded scope. This is the
  runtime FPU state [ADR-0063](../decisions/0063-eylem-determinism-contract.md) requires of eylem threads; until
  now no test checked it.
- The probe is `crd/core/fp_environment.hpp`, a leaf every module can use: `read_fp_environment`,
  `declared_fp_environment`, `write_fp_environment`, `ScopedFpEnvironment` and `fp_differences`, which names each
  differing field (flush-to-zero, denormals-are-zero, rounding, exception masks, x87 precision, rounding, masks and
  the other bits) and reports a field read on one side only as `Unread`, never as a match. MXCSR is read on every
  x86-64 compiler; the x87 word only where GNU inline assembly exists (gcc, clang, clang-cl; MSVC has none on x64
  and emits no x87 arithmetic). AArch64 reads nothing yet: its FPCR belongs to JOBS-PORT.
- At the crd-jobs boundary every job starts in the declared mode, whatever mode the thread that submitted or runs it
  is in; a mode a job sets is its own, kept across its waits and thread migrations and gone when it ends; no job
  changes the mode of the thread that ran it. Windows already held this (its fiber switch saves MXCSR and the x87
  word and a fiber restarts from a frame holding the defaults). The Linux switch saved neither: its comment called
  both caller-saved, but the x86-64 psABI makes their control bits callee-saved. A job's flush-to-zero or rounding
  mode therefore reached the worker's scheduler stack and every later job on that thread, a fresh fiber inherited
  whatever mode its thread had, and a waiting job resumed in the resuming thread's mode. `[diag10c]` cases in
  crd-jobs-tests caught all of it on linux-gcc-debug (all four failed, thread 0 itself ending in a job's mode);
  `fiber_switch_lin64.S` now saves and restores both registers and `fiber_init_stack` writes the declared values
  into a fresh fiber's frame (now 72 bytes, keeping `crd_fiber_start`'s entry alignment). Both platforms take the
  defaults from the probe's constants.
- Every job runs on a fiber (`WorkerPool::run_job_in_fiber`, from the worker loop and from the main thread's pump
  and wait); the synchronous `Scheduler::execute_one`/`run_job` is called only by the white-box scheduler tests, so
  the fiber switch is the only boundary to hold.
- Not yet settled (the rest of this row): value-domain probes (NaN, infinities, subnormals, signed zero and ranges
  with per-domain allowances), the generation-attributed wrong-result oracle at the CEIR boundary, shape and stride,
  units and time-domain, lifetime accounting beyond heap bytes, queue/VRAM pressure and budgets, warm/cold, soak,
  reload and shutdown workloads with one stressor at a time and seeded injection, slowdown measurement before
  classification, device-reset and host-pressure evidence, and the mode probe at other boundaries (an audio callback,
  a CEIR plan run, third-party library calls). (2026-10-11: the value-domain probes, the oracle and the CEIR plan run's
  mode are settled in the next block.)

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10c-value-domains-and-result-oracle.md)):
- A value domain is data a boundary declares: `crd/core/value_domain.hpp` (crd-core). A `FloatDomain` says, class by
  class, which special values it allows (NaN, +Inf, -Inf, subnormals, -0) and a closed range [minimum, maximum]; the
  default is "finite" (no NaN or infinity; subnormals and -0 allowed; no bound), and named domains cover the cases
  where a special value is right: `any` (NaN encodes a missing sample), `distance` ([0, +Inf], +Inf for a ray that
  hits nothing), `unit-interval` ([0, 1]). An `IntDomain` is a closed i64 range. A domain whose bound is a NaN or whose
  minimum exceeds its maximum is invalid and the probe judges nothing against it, saying so.
- The range is numeric: -0 equals 0, an infinite bound admits that infinity when its class is allowed, and a NaN has
  no place on the line, so only its class is judged. Several violations of one value are all reported (a disallowed
  -Inf below a finite minimum carries both).
- The probe reads bit patterns only and does no floating-point arithmetic or comparison, so no verdict depends on the
  caller's numerical mode: under denormals-are-zero a float comparison sees a subnormal as zero and a negative
  subnormal as not below 0, and a classifier built on `x == 0` reports a subnormal as a zero. A binary32 value is
  classified from its own bits (its subnormals are normal binary64 values) and widened to binary64 exactly before it
  is compared with the bounds. `probe_values` reports the values checked and violating, the union of violations and
  the first offender's index, violations and bit pattern; it never changes a value.
- The wrong-but-noncrashing oracle at the CEIR boundary is `crd/ceir/cook/result_oracle.hpp` (crd-ceir-cook). It
  judges the results of a run that succeeded against an independent expectation: an `IntDomain` invariant and, when
  given, reference values computed another way (a closed form, a slower algorithm, a recorded good run). The verdict
  is the first offending result: CountMismatch (another number of results than the reference), OutOfDomain (the
  domain is checked first at each index) or DiffersFromReference; Agrees when none, InvalidDomain when the expectation
  cannot judge. A wrong result is attributed by `ReloadSet::locate_result` to the generation the caller's handle names,
  never to whatever is installed now: asset, generation, Current/Retiring/Gone, content hash, and the op that defined
  that result in that generation's own Context (`file:line:col op#id`; the closing `func.return` when the result is a
  parameter or past its operands; no op for an entry the generation does not define). That op produced the value; it
  need not be the op an edit changed. A run that failed is not judged: its error already names the op it blames.
- The CEIR executors are integer-only (wrapping i64), so the oracle uses the integer domain, and a CEIR plan run has
  no floating-point mode to probe; the float domains wait for the tensor and audio boundaries, the next clauses.
  (2026-10-11: the tensor and audio boundaries, shape/stride and units/time-domain are settled in the next block.)

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10c-tensor-and-audio-boundaries.md)):
- Shape and stride are a claim a boundary checks before it walks a view: `crd/core/layout_probe.hpp` (crd-core, so
  every module's views inherit it). `probe_layout` reports, class by class, a rank past the boundary's bound, an
  element count (or its byte size) past the u64 range, an addressed element's offset (in elements or bytes) past the
  i64 range, an addressed element outside the storage the caller names, and a proven overlap. An empty view's other
  extents must still multiply out, because a compact layout of the same dims is computed from them. Offsets are formed
  per dimension with checked arithmetic, never by multiplying along. Storage the caller does not name is reported as
  unchecked, never as inside. Overlap is reported only when proven: a zero stride on an extent above 1, or two strides
  where the smaller divides the larger and its extent reaches the quotient, overlap for certain; strides that nest are
  disjoint for certain; anything else is "not proven" and never an overlap. A proven overlap is a violation only for a
  view the boundary writes through: a broadcast (stride-0) read is valid.
- `probe_strided_values` walks a strided view whose elements are one or more scalar lanes and judges every scalar
  against a `FloatDomain` from its bits, naming the first offender by its logical coordinates, its row-major index and
  its lane (1 for a complex value's imaginary part), never by a physical offset. It walks only a layout the probe
  passed, so it forms no out-of-range offset, and no verdict depends on the caller's numerical mode.
- The tensor boundary is `crd/hesap/tensor/probe.hpp`: `probe_tensor_layout` (a const view is a read) and
  `probe_tensor_values` for f32, f64, c32 and c64 views. Integer and mask tensors are not float domains and have no
  value walk. DLPack import, the boundary where another framework's tensor arrives, now routes the producer's shape
  and strides through the layout probe. Defects found and fixed in `wrap_dl_tensor`: an element count that wraps
  (`[2^32, 2^32, 2]`) imported as an empty tensor; an element whose offset is past the i64 range (`[3]` with stride
  2^62) imported, making any walk undefined pointer arithmetic; NULL strides over an empty shape with large extents
  overflowed a signed product; a capsule with `ndim > 0` and no shape array was dereferenced; and a view whose first
  or last byte wraps the address space imported. Each is now BadInput. An expanded (stride-0) capsule and an empty one
  still import; the probe reports the expanded one's overlap.
- The time domain at the audio boundary is the sample rate. `GraphSourceBinding` declares the rate its samples were
  made at (0 = undeclared), and `check_source_binding` (`crd/audio/audio_boundary.hpp`) judges a binding Bound,
  RateUndeclared, Unbound, PartialFrame or RateMismatch against the graph's rate (`AudioGraphResource::sample_rate`,
  or the CEIR func's `sample_rate`, 48 kHz when absent). Both renderers refuse a binding that does not render; an
  undeclared rate renders exactly as before and is reported as unchecked, never as a match. A mismatch is refused, not
  resampled. The shape check found two defects: a buffer that is not a whole number of frames had its trailing samples
  silently dropped, and a looped source of less than one frame divided by zero (`f %= bframes`) and killed the
  process.
- The value domain of a rendered sample is `audio_sample_domain()`: finite, no bound (headroom above 1 before the
  master stage is legal), subnormals and -0 allowed. A NaN in one source reaches the bus and the probe names it.
- The mode probe at the audio callback is `render_audio_period`, the device's per-period work separated from the
  device: it runs the callback in the declared mode whatever mode the device thread is in, keeps a mode the callback
  sets inside its period, reports both as `FpDifference` bits, and probes the stereo the callback wrote. The WASAPI
  device counts divergent and non-finite periods (`fp_mode_divergence_count`, `nonfinite_period_count`). Defect found
  and fixed: the device handed the stereo callback a buffer interleaved at the endpoint's channel count, so a 5.1 or
  7.1 endpoint garbled the layout; a non-stereo endpoint now renders through a stereo scratch the device sizes in
  `start()` and gets the callback's stereo in its first two channels.
- Around third-party calls the witness is `call_keeping_fp_environment` (`crd/core/fp_environment.hpp`): it reports
  the `FpDifference` bits a foreign call left and puts the caller's mode back. A DLPack producer's deleter runs
  through it on every path (`DlpackImported::release`, which returns the bits, the destructor, and the spec-mandated
  disposal of a newer-major capsule). The GPU drivers' calls belong with the driver and device-reset evidence clause.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10c-lifetime-accounting.md)):
- Lifetime accounting beyond heap bytes is the ledger `crd/core/lifetime_ledger.hpp` (crd-core, below every module
  that must feed it). Five classes: FileHandle (an OS file handle or descriptor kept open past the call that opened
  it), LibraryHandle (a LoadLibrary/dlopen reference), MappedRegion (a reserved or mapped range, with its bytes),
  DeviceObject (a GPU resource or program carrying a Cerid identity) and CallbackRegistration (a callback, subscriber,
  sink or observer registered with an owner). Per class it keeps acquired, matched releases, live, the high water and
  live bytes. Recording is relaxed atomics in a fixed constant-initialized table: no lock, no allocation, valid from
  any thread and during static destruction. A snapshot is a set of independent reads, meaningful once the bracketed
  workload has finished.
- The ledger checks itself. A release it cannot match (nothing of the class is live, or more bytes than are live)
  changes nothing and is counted as unmatched; a window over it is Underflowed, never a wrapped or clamped count. A
  window between two snapshots is, worst first, Underflowed, Outstanding (a class holds more live count or bytes),
  Drained (it released earlier acquisitions) or Balanced, with per-class masks and signed deltas, and
  `format_lifetime_report` renders one line ("outstanding: file-handle +1, mapped-region +1 (+4096 bytes)").
- Wired sites, each recording after the OS or owner granted and after it took back (a failed free stays live, as it
  does in the process): `vm::reserve`/`vm::release` and the crd-jobs fiber stacks (MappedRegion, with the reserved
  size including the guard page); `DynamicLibrary` open and destructor (LibraryHandle); `log::FileSink`'s open, close
  and rotation (FileHandle); the process-wide `gpu::identity_registry()` mint and retire of Resource and Program
  identities (DeviceObject; a Pass identity names a recorded pass and is not counted; a registry built without the
  feed, such as a test's own, records nothing); and, as CallbackRegistration, `app::EventBus` subscriptions (now
  move-only, released by unsubscribe, a move-assignment over a bus holding them, or destruction),
  `platform::FileWatcher` entries (the same), `ResourceManager::subscribe_reload` (released by unsubscribe or the
  manager's destruction), installed log sinks (released by `clear_sinks` and `shutdown`) and the jobs observer
  (installing over nothing acquires, clearing releases, replacing keeps).
- Not recorded, by rule: a handle opened and closed inside one call cannot outlive it, so the RAII streams of
  `platform::fs`, `ceir::cook`'s bounded file reads and writes, the perf bundle, capture, export, diag-asset, command,
  doctor and flight-command file writes, crd-jobs' topology reads and the module snapshot of `module_enumeration` are
  scoped; a wired test judges them through the OS oracle below when it needs to. `crash.cpp` is not wired: its dump
  files are written on the fatal path, which must touch no more shared state, and its handler thread and events live
  for the process. Not yet recorded: the window, the WASAPI device's COM objects and event, worker and logger threads,
  and native device objects without a Cerid identity (devices, queues, fences, swapchains); they belong with the
  shutdown workloads that exercise their owners. (2026-10-12: threads are recorded since the workload slice, and the
  window, the WASAPI device's objects and the native device objects since the queue-depth slice; see the last block.)
- The independent oracle is `crd/core/os_resource_probe.hpp`, which asks the OS, sharing no code with the wired sites.
  `os_file_open_in_process(path)`: Windows reads the process's own handle table (NtQueryInformationProcess,
  ProcessHandleSnapshotInformation, Windows 8 and later), keeps the handles of the File object type that are disk files
  and compares their normalized final paths with the path's, excluding the probe's own attribute-only handle; Linux
  compares the (device, inode) of every descriptor in `/proc/self/fd`. Another process's handle (a scanner, an indexer)
  never counts. `os_range_mapped(base, bytes)`: Windows walks VirtualQuery (any state but MEM_FREE); Linux reads
  `/proc/self/maps`; the answer is Present, Absent or Partial. `os_range_no_access(base, bytes)` counts only what
  nothing may touch (MEM_RESERVE or PAGE_NOACCESS; `---` in the maps) and judges a release: the freed range is where
  the OS places the process's next mapping, which a sanitizer runtime makes at any instruction (2026-10-24: a TSan
  runtime mapping over a released reservation), and that mapping is accessible. A path that cannot be identified, an
  empty or wrapping range, or another OS is Unavailable with a reason, never Absent. The handle-table read lives in
  pages from the OS, not the heap; it queries every File-type handle (the console and ctest pipes included), so it is
  for quiet test and investigation processes, where no synchronous pipe read is pending on another thread. Libraries
  have no oracle: both OSes reference-count a module and expose no per-process count, so the library cases are judged by
  the ledger and the API's behaviour alone. Device objects are held to the registry's own `live_count`, a second count
  the ledger must agree with.

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10c-queue-pressure.md)):
- Queue and budget pressure is a reading the owner of a bounded resource reports and a probe judges:
  `crd/core/budget_pressure.hpp` (crd-core). A `BudgetReading` carries the bound (or that there is none), what is
  held now, the most held at once where the owner keeps it, the requests refused for want of room and the times a
  soft bound was left exceeded. `pressure_level` judges one amount: Unread (no reading), Unbounded (reported, never
  judged), Within, Near (amount × 1000 ≥ bound × threshold, 900 by default, computed exactly without an overflowing
  product), Full (at the bound; a bound of 0 is full when empty) or Over. `pressure_report` judges the window between
  two readings of one resource, worst first: Exhausted (a refusal in the window), Overrun (an overrun counted, or the
  window held more than the bound), Saturated (it reached the bound), Near, then Relaxed only when a kept high water
  proves the peak below Near; otherwise Undetermined (a high water the owner does not keep, or one from before the
  window that is at or above Near). Readings that cannot be compared (unread, unbounded, another bound, a monotonic
  count that went back) are Unjudged. The peak is reported as a range: at least the larger end, raised to the high
  water when it rose in the window, and at most the high water. Integer arithmetic only; one rendered line per window.
- At the crd-jobs boundary `crd::jobs::pressure_snapshot()` reports the completion counters (`max_counters`, with a
  high water kept beside the acquired count), each fiber tier (its count, acquired, peak and its own refusals), each
  injection queue by priority (capacity, depth and refusals; no high water, which would cost every push a read of the
  consumers' position, so a window over an injection queue is never called Relaxed) and the pinned lanes as one budget
  (the per-thread capacity, the fullest lane, the highest lane peak, all lanes' refusals). Each pool raises its peak
  just after the count it bounds, so a reading reports the high water as at least the count it read (2026-10-24: a
  fiber reading showed 9 in use with a high water of 8); the lifetime ledger's snapshot does the same for `live`.
- Defects found and fixed. A job pushed into a full injection queue, or into a full local deque, was guarded only by
  `CRD_ASSERT_MSG`: with asserts compiled out the job was dropped, its counter never reached zero and its waiter hung
  (resume jobs go through the High queue, so a dropped resume strands a parked fiber). And each thread's pinned work
  was a single slot: a second job pinned to a thread before that thread drained the first asserted where asserts are
  on and silently overwrote the first elsewhere, and concurrent producers raced on the plain slot. Authored CEIR
  reaches it: two `task.main_thread` tasks launched before either is awaited (the CEIR host lowers each to a job
  pinned to thread 0). On the old code the new crd-ceir-host-tests case and the first crd-jobs-tests case hit the
  assert on win-debug, and that jobs case hung on win-shipping until it was killed at 60 s. The pinned lane is now a bounded MPMC queue
  (`Config::pinned_queue_capacity`, default 64, a power of two, checked by an always-on fatal at init), drained by its
  thread before anything else; a full pinned lane, injection queue or local deque is counted and is an always-on
  fatal naming the resource ("pinned lane full", "injection queue full", "local deque full"), as counter and fiber
  exhaustion already were. A job is never dropped; a refusal is visible to a handler intercepting the fatal, which
  is where a refusal count can be read ([ADR-0033](../decisions/0033-jobs-implementation-architecture.md) amended).
- The ResourceManager's memory budget is soft: it never refuses a load, and when every resident block is pinned,
  held or loading it stays above the budget silently. `ResourceManager::memory_pressure()` reports the budget
  (unbounded at the default), the use, the high water of the use once a load's eviction (or a reload's
  re-accounting, which does not evict) settled, and the overruns: the settles that ended above the budget.
- Not settled here: the OS view of device memory (DXGI `QueryVideoMemoryInfo`, `VK_EXT_memory_budget`), GPU queue
  depth and the frame graph's transient budget readings belong with the driver/device-reset and host-pressure
  clause; the hang watchdog does not yet classify a stall from these readings. (2026-10-12: device memory and the
  transient budget are settled in the device and host pressure block below; queue depth is not yet.)

Settled (2026-10-11, [session](../sessions/2026-10-11-diag-10c-workloads.md)):
- Workloads are driven and judged by `crd/core/workload_probe.hpp` (crd-core, so every module can run its own).
  `run_workload` runs `cycles` times: setup, `iterations` iterations, teardown. Iteration 0 of a cycle is cold (just
  after setup: nothing cached or resident: a fresh job pool's first fibers, a fresh resource manager's first read of
  the pack from disk); the rest are warm. Each iteration reports success, a signature of its result and, when it knows
  it, the signature it should have; one that does not know it must equal the reference, the signature of cycle 0's
  cold iteration. A lifetime-ledger window brackets every cycle from before setup to after teardown.
- Seeded failure injection. Iteration i of cycle c is injected exactly when `workload_injects(seed, permille, c, i,
  iterations)` says so: a pure function, never the cold iteration nor the last, only odd iterations (so a clean one
  follows each), and splitmix64 of (seed, cycle, iteration) below the share. The spec's hook arms a fault before the
  iteration and reports afterwards how many fired. An injected iteration must report failure and keep the result it
  says it keeps; the next one must succeed with its expected result.
- The verdict is the worst finding, first occurrence located by cycle and iteration: SetupFailed, Diverged (a
  signature differs from what it should be: a wrong result that did not crash), FaultMissed (a fault fired and the
  iteration reported success), NotRecovered, IterationFailed, Leaked (a cycle window Outstanding or Underflowed),
  Stalled, FaultNotFired (an injected fault never fired, so the run proves nothing about that path), else Clean.
- One changed stressor at a time. The stressors are enumerated (`Stressor`: iterations, cycles, threads, fault plan,
  memory budget, reload cadence, payload size, injected delay) and a run carries its `StressorSet`.
  `compare_stressors` answers Identical, OneChanged (naming it) or SeveralChanged. `attribute_finding` blames a
  variant's finding on a stressor only against a clean baseline that differs in exactly that one: otherwise
  BaselineFailing or NotOneStressor, and NoNewFinding when the variant is clean.
- Progress. `progress_report` judges readings (when, how much done) in time order: Stalled when no advance is seen for
  longer than the bound (a reading without an advance counts toward the gap; readings after the target are not
  judged), Regressed when the count goes back (the counter itself is wrong), Unfinished when it never stalled but ended
  below its target, Progressing otherwise, and Unmeasured with fewer than two readings, no bound, or time going back.
  A workload's progress is read at the end of every iteration, counting the work units of successful ones, so setup and
  teardown fall inside the gaps the bound must cover.
- Slowdowns are measured before they are classified. `slowdown_report` compares two windows of durations (at most
  1024 samples each) by nearest-rank quartiles with integer arithmetic only (products saturate). Unmeasured: fewer than
  8 samples in either window, or a baseline median below 1000 ns (too short to time). Noisy: the baseline's
  interquartile range exceeds its median. Slower: the candidate median exceeds the baseline's by more than the
  tolerance (250 permille) AND the candidate's lower quartile lies above the baseline's upper quartile; Faster the
  mirror image; Inconclusive when the median moved past the tolerance but the quartiles overlap; Steady otherwise. A
  workload's own report compares the first and second halves of its warm, clean, non-recovery iterations.
- Hosted lanes judge results, lifetimes and progress, never speed: a wall-clock verdict depends on the runner, and CI
  has no retry. The workloads print their one-line report (timing included) on every run as evidence; the
  classifier's verdicts are each held to synthetic durations, and the stall cases sleep far longer than their bound.
  The progress bound is the one wall-clock judgment the hosted lanes make; it is 5 s for the real workloads, far above
  any init/shutdown or reload seen on a sanitizer lane.
- Threads joined the ledger as a sixth class, Thread: an OS thread the engine started and must join. Wired: the crd-jobs
  workers and hang watchdog (init/shutdown), the asynchronous logger's worker, an inspect host's run thread (released
  when a later load, start or wait, or the destructor, joins it), the diagnostic panel's request worker and the WASAPI
  render thread (also when a failed start joins it). Not wired: the crash handler thread, which lives for the process.
  The OS oracle is identity, not a count: `os_current_thread_identity` read inside a thread gives its OS id and start
  time (Windows: thread id and creation FILETIME; Linux: gettid and the starttime field of
  `/proc/self/task/<tid>/stat`), and `os_thread_alive` answers Present while that thread runs in this process and Absent
  once it has ended, its id names another process's thread or a later thread (start time differs), or it is a zombie.
  A process thread count is no oracle: the OS loader and thread pool start and retire threads of their own.
- Workloads in place: crd-jobs-tests runs a soak across jobs init/shutdown cycles (3 cycles of 64 parallel sums with a
  closed-form answer, 2 threads, and the one-stressor variant of 4 threads), requiring every sum, a balanced window per
  cycle with the workers and the watchdog counted, every worker the OS saw running gone after its cycle, and progress
  within a 5 s bound; a variant with one 7 s delay stalls and is attributed to Delay. crd-resources-tests runs 3
  cycles of a fresh ResourceManager (mount, cold load, 23 reloads of new content, destruction) as the baseline, a
  memory-budget variant (clean, its pressure reading counting overruns) and two fault-plan variants arming a failed or
  a short read of every injected reload's pack on the real I/O seam: each injected reload fails, keeps the last good
  mesh and recovers on the next; a shipping build refuses the arms and the variant answers FaultNotFired.
- Defect found and fixed by the jobs soak. On the default (shared-semaphore) wake path a job pinned to a worker posted
  one token and woke one arbitrary sleeper; when that was another worker, it took the token, found nothing it could run
  and slept again, and the target slept on with the job in its lane, so the waiter hung (the 4-thread variant hung on
  its second run on win-debug; a dedicated case pinning to each idle worker of an 8-thread pool lost the first job). A
  pinned push now wakes every sleeper behind a sequentially consistent fence, and a worker announces it is going to
  sleep before its last look at its own lane; a worker about to sleep that finds an announced sleeper holding pinned
  work wakes every sleeper instead of sleeping, so whoever takes the push's token wakes the target
  ([ADR-0033](../decisions/0033-jobs-implementation-architecture.md) amended). The targeted path is unchanged.
- Defect found and fixed by the logger-thread case under linux-clang-tsan: `log::flush()` ("block until all queued
  records are written") waited only for an empty queue, but the worker pops a record before it takes the sink lock, so
  a flush could take the sink lock first and return before that record was written. The worker now counts the record
  it is delivering (under the queue lock), and `flush()` and `flush_for()` wait until the queue is empty and nothing is
  being delivered.
- Not settled here: driver/device-reset and host-pressure evidence (with the GPU side of pressure and the mode witness
  around driver calls), and the window, the WASAPI device's COM objects and event, and native device objects without a
  Cerid identity in the ledger; they belong with the device clause, whose owners the hosted lanes can exercise only
  in part (no display or render endpoint there). (2026-10-12: the evidence, the GPU side of pressure except queue
  depth, and the mode witness are settled in the block below; the ledger owners are not yet.)

Settled (2026-10-12, [session](../sessions/2026-10-12-diag-10c-device-and-host-pressure.md)):
- Host pressure is a reading of the OS's memory, `crd/core/host_pressure.hpp` (crd-core): `read_host_memory` reports
  three `BudgetReading`s. Physical memory is bounded by the usable RAM, with in use = that minus what the OS reports
  available (Windows `GlobalMemoryStatusEx`; Linux `/proc/meminfo` MemTotal and MemAvailable), no high water. The
  commit charge is bounded by the commit limit only where the OS enforces it: Windows always (its system peak from
  `GetPerformanceInfo` is the high water), Linux only in strict overcommit mode (`vm.overcommit_memory` 2); otherwise
  it is unbounded, reported and never judged. The resident set is this process's own (working set or VmRSS) with its
  peak (PeakWorkingSetSize or VmHWM) and no bound. A reading the OS could not give is unread with a reason, never zero
  use; no heap memory is allocated, so it can run on a failure path. `host_physical_total_cross_check` asks a second
  interface (`GetPerformanceInfo`, `sysinfo`) for the usable RAM, the independent check of the physical bound.
- Device memory is `IGpuContext::device_memory()` (appended at the vtable end; default: unread with a reason),
  `crd/gpu/device_pressure.hpp`. The bound is the budget the OS grants this process and in use is the process's use,
  per adapter whichever device of the process allocated: DX12 asks DXGI (`IDXGIAdapter3::QueryVideoMemoryInfo`, the
  local and non-local segment groups of node 0); Vulkan enables `VK_EXT_memory_budget` whenever the device advertises
  it and sums the device-local heaps into `local` and the others into `non_local`. The budget is soft, so use past it
  reads Over. A segment group or heap with a zero budget and zero use is absent, not full: WARP answers 0/0 for its
  non-local group and it stays unread. Observed: DXGI usage moves by at least a committed 256 MiB buffer and back on
  the RTX 4070 Ti SUPER and on WARP; `VK_EXT_memory_budget` does the same on the RTX driver; Mesa's llvmpipe (the
  hosted Linux lanes' device) advertises the extension but reports a heap usage that does not follow the process's
  allocations (constant across a 256 MiB allocation and its free), so its reading is the driver's estimate and its
  test names that instead of calling it tracked.
- The frame graph's transient budget is `IFrameGraph::transient_pressure()` (appended at the end): the bound is
  `set_memory_budget` (unbounded at 0), in use the last build's post-aliasing footprint, the high water the largest
  footprint any build planned (a refused build's included: it was allocated before the check) and each refused
  build counts once. The bound is hard, so it never overruns. `gpu.resources` now reports both: each context's
  device memory (status read or unavailable with a reason; per segment a budget, a usage and the usage's pressure
  level, or "unavailable") with a `device_memory_read` count in the summary in place of `heap_usage: "unavailable"`,
  and each frame graph's transient budget, high water, refusals and level (or `transient_pressure: "unavailable"`).
- Defect found and fixed by the transient-budget cases: a second `build()` of one recording placed the transients again
  over the first build's. On DX12 it overwrote the wrappers that hold the placed resources and leaked them, with their
  heaps and the device behind them: DXGI usage stayed 32 MiB up after the graph was destroyed, and the leaked device
  made the next test's validation-enabled device creation fail. On Vulkan the images are created when declared and
  bound in the build, so a second build bound memory to images already bound; on the RTX the VK budget saw a 16 MiB
  leak. A frame graph now builds once per recording on both backends: once a build has placed transients (whether it
  then succeeded or the budget refused it), another `build()` without `reset()` is refused, logged, and is not a
  budget refusal. The documented recovery from a budget refusal is reset, record again, build.
- The numerical-mode witness around code the engine does not own on its threads: `crd/gpu/driver_witness.hpp`.
  `witness_driver_call(kind, label, call)` reads the thread's mode before and after the call, puts the earlier mode
  back when they differ, counts calls and divergences per kind (process-wide relaxed atomics) and keeps the first
  divergence's fields, kind and label; it returns what the call returned and never judges the caller's own mode. Wired:
  CreateInstance (`vkCreateInstance`), CreateDevice (`D3D12CreateDevice` in the device scope every DX12 context uses,
  `vkCreateDevice`), CompileShader (dxc for DXIL and for SPIR-V, shaderc), Submit (`ExecuteCommandLists` and
  `vkQueueSubmit` in the two completion seams) and Present (`IDXGISwapChain::Present`, `vkQueuePresentKHR`). No
  divergence was seen on the RTX driver, on WARP or on llvmpipe.
- Device-reset evidence and attribution. Each backend takes a `GpuLossPressure` when it records a loss (DX12: every
  recorded removal; Vulkan: the first loss of a device): the host reading and that adapter's device memory, with the
  loss's own sequence (`dx12_last_loss_pressure`, `vk_last_loss_pressure`; the adapter still answers after its device
  is removed). `attribute_device_fault` refuses to call a failure an engine defect without a discriminating
  reproduction: a simulated failure is always Injected; a failure the same inputs reproduce on another configuration
  (another adapter, the software adapter, the other backend) is an EngineDefect; otherwise a reading at the failure
  that is Full or Over (physical memory, an enforced commit limit, the device's local or non-local budget) makes it
  PressureCoincident, a reproduction that did not fail NotReproduced, and anything else (no reproduction, or one only
  on the same adapter) Undiscriminated. A real loss stays the user-gated hardware route of DIAG.7b(h2) and DIAG.7c(h);
  the injected losses here prove the evidence path and are attributed Injected.
- Not settled here: GPU queue depth (an in-flight count at the completion seams) and the native owners in the
  lifetime ledger (the window, the WASAPI device's COM objects and event, and native devices, queues, fences and
  swapchains without a Cerid identity) belong together with the shutdown of those owners; the hang watchdog does not
  yet classify a stall from the pressure readings. (2026-10-12: queue depth and the native owners are settled in the
  next block; the watchdog item stands.)

Settled (2026-10-12, [session](../sessions/2026-10-12-diag-10c-queue-depth-and-native-owners.md)):
- GPU queue depth is `crd/gpu/queue_pressure.hpp` (crd-gpu-context, device-free, keyed by opaque device, queue and
  timeline handles). A step is in flight from the completion seam that queued it until a seam observed it complete.
  A queue runs its steps in order, so a timeline observed complete through a value retires the step that signalled it
  and every earlier step on its queue, and none on another queue. A DX12 fence signals increasing values; a Vulkan
  fence is binary (value 0, observed through every value); a submission with no timeline (a Vulkan submit without a
  fence) is retired by a later timeline on its queue or by an idle wait; a bare `dx12_signal` is a step too. The
  reading is the engine's own view: work the GPU finished that no seam has looked at still counts, which is what a
  missing wait, an owner destroyed with work queued or a CPU running ahead of its frames leaves behind.
- Every other end is counted, never hidden. A device recorded lost, a queue or device forgotten, and a timeline that
  starts again while it has steps pending (a fence created again at a reused address; a binary fence re-armed before a
  seam observed it) abandon their steps. A completion observed with nothing pending is unmatched (a second wait on a
  signalled fence is legal). The tracker is a fixed table under one mutex (512 steps, 64 queues, 32 devices); a step it
  cannot hold is untracked, and that device's reading is then unread rather than low. Per device the counts are
  submitted, retired, abandoned, in flight, the high water, untracked, unmatched, live queues and lost; they start
  fresh once the device, or its last live queue, is forgotten.
- Fed by the seams only: DX12 `dx12_submit` and `dx12_signal` after their Signal, `dx12_wait` on success (also when
  the fence had already passed the value), every removal path (`stop_failed_device`, the injected loss) marks the
  device lost, and each context forgets its own queue in its destructor; Vulkan `vk_submit` on success, `vk_wait` and
  `vk_wait_complete` on success, both idle waits, the first recorded loss of a device, and `vk_forget_device`. The CKIR
  DX12 test backend (crd-kir-dx12) submits outside the seams and is not in the depth.
- Keys are addresses, so a device's counts must end with the device. A Vulkan device is forgotten before
  `vkDestroyDevice`. A DX12 device has no destroy call, so the seam attaches an `IUnknown` with
  `SetPrivateDataInterface` the first time it reports a step or a loss on a device (serialized, so two first
  submissions cannot replace each other's); D3D12 releases it when the device is destroyed, and its final release
  forgets the device. Seam callers that never forget a queue (the validation cases drive the seam directly on devices
  of their own) therefore cannot leave a lost reading for a later device created at the same address.
- The reading is `IGpuContext::queue_pressure()` (appended at the vtable end; default unread with a reason): the
  device's counts as a `BudgetReading` that is unbounded (a bare queue has no bound, so its depth is reported, never
  judged against one), in use = in flight, high water tracked; unread while the device is lost (its steps were
  abandoned, not completed) or a step went untracked. D3D12 hands one device per adapter to the process, so a DX12
  context's reading covers the raster and compute contexts on its adapter. `gpu.resources` adds `queue_in_flight`,
  `queue_high_water`, `queue_submitted`, `queue_retired`, `queue_abandoned` and `queues` to each context item (or
  `queue_depth: "unavailable"` with `queue_reason`), and `queue_depth_read` to the summary.
- Observed: a synchronous context (compute, ray tracing, work graph, the raster one-shot path) never leaves a step in
  flight; the DX12 frame graph's deferred wait leaves its frame in flight until the next execute or reset; the Vulkan
  frame graph's two-slot ring leaves at most two (its high water is exactly 2 over four frames), and its teardown
  observes both.
- The native owners are two more ledger classes, eight in all, which fill the report's u8 masks (a ninth widens them
  first): NativeGpuObject (an instance, a device reference, a created queue, a fence or a swapchain without a Cerid
  identity) and OsObject (a window, an event, a COM interface reference). Wired: one device reference per
  `Dx12DeviceScope` (declared before its context's device, so released after it), each created DX12 command queue and
  fence (compute, ray-tracing, work-graph and raster contexts, the present surface's fence), the DXGI swapchain and
  every fence event; the crd-kir-dx12 backend's device, queue, fence and event; the Vulkan instance and device, every
  fence (compute, the present ring, upload batches, frame-graph slots and the async fence) and every swapchain,
  including the one a resize replaces; the GLFW window (create, destructor, move-assignment over a live window); the
  WASAPI enumerator, endpoint, audio client, render client and period event. `crd::LifetimeHold` records an
  acquisition whose release is implicit (a COM smart pointer, a member destroyed with its owner): declared before the
  member, it is released after it, and a move transfers it.
- Not recorded, by rule: Vulkan queues (retrieved with `vkGetDeviceQueue`, not created, so the DX12 and Vulkan counts
  differ by design), semaphores, command pools, allocators and lists, descriptor heaps and pools, pipelines, query
  heaps, the Vulkan surface, the DXGI factory and COM references a call opens and releases itself; each is owned by an
  object the ledger already counts or is scoped to one call. A GPU resource or program with a Cerid identity stays a
  DeviceObject.
- Defect found and fixed by the audio restart case: `AudioDevice::start()` after `stop()` obtained a second enumerator,
  endpoint, client, render client, mix format and event over the first set and initialized COM once more, leaking the
  stopped session; a start now releases a held session first.
- Defect found and fixed by the Vulkan lost-queue case under ASan: a simulated loss (`vk_inject_next_result`, or the
  DIAG.9b device-loss fault) leaves a device that still runs what it was given, while every later seam wait answers
  without the driver, so a frame graph and raster context torn down after it destroyed objects in-flight work still
  used (lavapipe leaked a worker allocation, which fails the case under LeakSanitizer). After recording a simulated
  loss the seam now drains the real device once with a direct `vkDeviceWaitIdle`, so nothing it was given still runs,
  as after a real loss. A real loss is never called again.
- The shutdown workloads (results and lifetimes only, never speed): three DX12 cycles of a compute context created,
  used four times and destroyed on the default adapter; three Vulkan cycles of instance, device and compute context;
  window create and destroy cycles; audio stop and restart. Each cycle's ledger window is Balanced and every value read
  back is the one written. The OS judges the window (FindWindow by a unique title, and the process's USER-object count,
  which returns to its value before the platform context once the context is gone); the GPU counts are held to what
  each owner's code creates (a compute context: a device reference, a queue, a fence and an event; a DX12 present
  surface: a swapchain, a fence and an event; a Vulkan present surface: a swapchain and two ring fences), since neither
  OS exposes a per-process count of D3D12 or Vulkan objects. A context, window or audio device that cannot start (no
  Vulkan device, no display, no render endpoint) must leave the ledger balanced.
- Not settled here: the hang watchdog does not yet classify a stall from the pressure and depth readings.

<a id="diag-11a"></a>
## DIAG.11a — reusable host integration and shipping profiles

Integrate lifecycle-managed diagnostics into existing app/headless/sandbox hosts through optional public composition.
Exercise a thin external application consuming installed modules; avoid sandbox-only setup. Default policy is local,
bounded and does not expose a remote debugger. Startup chooses supported modes before device/tool initialization;
dynamic policy changes say when restart is required. Use normal authorable configuration, not hidden C++ algorithms.

Acceptance: a small application runs, deliberately fails, emits an attributable bundle and is investigated without
editing engine source. Add examples for allocator selection, jobs, reload and capture. Verify profiling disabled,
optimized profiling and diagnostic binaries; package symbols/policies intentionally and test clean-machine consumption.

Settled (2026-10-12, [session](../sessions/2026-10-12-diag-11a-reusable-diagnostic-host.md)):
- One lifecycle for every host: `crd::perf::DiagnosticHost` (`crd/perf/diagnostic_host.hpp`, crd-perf). It is the
  sandbox's DIAG.9b flight host moved into the engine and widened: it owns the crash handler, the flight recorder and
  its triggers (assertion, fatal invariant, jobs starvation and hang, frame hitch, allocation failure at a wrapped
  allocator), the flight bundle writer, the fault injector with the I/O and GPU seams' hooks, and the flight.* and
  fault.* commands. `sandbox/src/flight_host.*` is deleted; its cases moved to crd-perf-tests against the engine class.
- The modes are an authored asset, kind `diagnostic-host` (`*.host.toml`), parsed like the other diagnostic assets
  (header first, every other key optional, an unknown key or a bad value refused by name, a refusal leaves the profile
  untouched): `crash_handler`, `flight` and `fault_injection` take `"off"`, `"on"` (used when this build can, reported
  Unavailable otherwise and the run continues) or `"required"` (the run is refused); `crash_dir`, `flight_policy`,
  `capture_dir`, `max_bundles` (1 to 1000), `command_grant`, `allow_remote`, `watchdog_period_ms` (0 to 60000).
  Relative paths are relative to the working directory, as command-line paths are. `assets/diag/host_default.host.toml`
  is held equal to `DiagnosticHostProfile{}`; `host_investigate.host.toml` adds injection and a record/inject grant.
- The default policy is local and bounded: crash dumps and bundles stay on the machine, the service may only read, no
  fault is injected, and a grant naming remote-enable, upload or process-memory is refused unless `allow_remote = true`.
  Further rules: inject in the grant needs injection on or required; injection, a policy and a capture directory need
  the recorder; `crash_dir` is not empty while the crash handler is asked for.
- Startup chooses the modes before what depends on them exists: `start()` decides every mode before installing
  anything (a refusal leaves nothing behind), refuses once the jobs pool runs (LateStart: the watchdog window and the
  pool's handlers are part of the choice), and the host starts before devices and tools. Fault injection is available
  exactly where the seams are compiled (`fault_seams_compiled()`); the crash handler where the platform has a backend
  and its install succeeds. flight.* is served while the recorder runs and fault.* while injection runs, so a shipping
  build reports injection Unavailable (the doctor says "compiled out") and serves no fault.* command.
- Dynamic changes say when a restart is required: `classify_diagnostic_host_change` names the differing fields;
  `capture_dir`, `max_bundles` and `flight_policy` apply live, every other field at the next start. `apply()` answers
  Applied, Unchanged, RestartRequired (nothing applied, the fields named), Busy (a capture is pending, so the policy
  cannot change; nothing applied), Refused or NotStarted. Everything that can refuse is checked before anything
  changes, and the policy file is re-read, so an edited file at the same path is a live change.
- The hosts: `Application` takes the host in `ApplicationDesc::diagnostics` (a started host owns the crash handler and
  `install_crash_handler`/`crash_dir` are ignored; every `tick()` is one of its frames, start to start, and delivers a
  capture whose window closed; `run()` gives the pool the host's watchdog window; an unstarted host makes the
  Application invalid). The sandbox starts it from `--diag-host <asset>` or the default profile, with `--flight-policy`,
  `--flight-dir` and `--diag-grant` as overrides held to the same rules, and passes it to its Application. ceridc,
  the headless host, starts it with `ceridc mcp --diag-host <asset>` before its pool and service and polls it after
  every request; without the flag nothing is installed, as before.
- The deliberate failure: `crd-diag-host-specimen` is a small application that starts the host from an authored
  profile file and fails a fatal invariant or a headless assertion; the profile's capture directory receives a bundle
  naming the trigger, the forty events before it, the replay recipe, this build and the specimen's own module identity
  (checked against the file on disk). Through ceridc, a capture pending when an MCP session ends is written at stop and
  `ceridc diag --command bundle.inspect` reads it back (its flight-record section, `same_as_host`, the ceridc module).
- Not settled here (the row's remaining clauses): the installed external consumer (only `Cerid::core` is packaged, so
  the host's family must be exported, symbols and the diagnostic assets installed intentionally, and a clean-machine
  consumer built); the examples for allocator selection, jobs, reload and capture on that consumer; the three binaries
  (profiling disabled, optimized profiling, diagnostic) shown for it; and an investigation route that reads a flight
  record's trigger and events, which `bundle.inspect` does not decode yet (it lists the section). (Amended 2026-10-12:
  all of these but the four examples are settled below; amended again 2026-10-12: the examples too, in the last
  block.)

Settled (2026-10-12, [session](../sessions/2026-10-12-diag-11a-installed-consumer.md)):
- The installed family: the package exports the diagnostic host and its closure (`Cerid::core`, `units`, `time`,
  `vm`, `log`, `containers`, `memory`, `jobs`, `toml`, `perf`, with `math` and `simd-flags`), installs each archive's
  symbols as `CeridConfig.cmake` declares them (`Cerid_DEBUG_INFORMATION`: the MSVC compile PDBs beside their
  archives, DWARF inside the GCC archives) and the authored diagnostic assets under `<prefix>/share/cerid/diag`
  (`Cerid_ASSET_DIR`). The packaging rules are [public consumption](public-consumption.md#the-diagnostic-family-diag11a).
- The thin consumer is `tests/support/package/diag-consumer`: a standalone CMake project that only calls
  `find_package(Cerid)` and links `Cerid::perf`, never part of the engine's build. `scripts/test-package-consumer.py`
  (the `crd-package-consumer` CTest of the public-check presets) builds it against the moved prefix after deleting the
  engine's build tree, the nearest local stand-in for a clean machine; the hosted public-check lanes are fresh machines.
- It runs and fails on purpose: `diag-consumer fail <profile>` starts the engine's `DiagnosticHost` from a profile the
  test authors from the installed `host_investigate.host.toml` (the installed flight policy, a crash directory and a
  capture directory of its own), prints this binary's capabilities through the host's own service
  (`diag.capabilities`), records forty orders and fails a `CRD_DIAG_INVARIANT`. The capture directory receives one
  flight bundle before the process dies. `CRD_DIAG_INVARIANT` now names its types fully, so an application needs no
  `cont` namespace alias of its own.
- It is investigated without engine edits: `diag-consumer inspect <dir> <bundle>` reads the bundle through
  `bundle.inspect`, which now decodes the flight record. Besides the sections, the build and the modules, it lists a
  `trigger` item (kind, code, file, line, message, scalars, sequence), a `flight` item (how the window ended, the policy
  generation and detail, the loss metadata, the replay recipe) and one `event` item per retained event, each saying
  whether it was recorded before or after the trigger; the summary says `flight_record`, and a record the decoder
  refuses (or an oversized or compressed section) is `flight_refused` with the reason, never silently absent. The test
  requires the trigger at the application's failing line with its code and message, the forty events before it in
  order, the window ended `fatal`, the replay recipe, `same_as_host`, and the application's own module identity. ceridc
  serves the same command, so the headless host gets the route too.
- A trigger's source path longer than its 128-byte field keeps its tail (the file name) after `...`, cut on a UTF-8
  boundary; before, it kept the head, so an application built in a deep directory lost the name of its failing file.
- The three binaries: the same run under the diagnostic profile (`win-debug`, `linux-gcc-debug`: asserts, profiling,
  fault injection active), the profiling-disabled one (`win-shipping`, `linux-gcc-shipping`: profiling not compiled,
  fault injection Unavailable and the run continuing) and the optimized-profiling one (`win-shipping-profile`; Linux
  has no such preset). The application's own build line and `diag.capabilities` (`basic-recording` and
  `fault-injection` compiled or not) are held to the profile. Only the Debug public-check presets run the CTest
  hosted; the shipping profiles are local evidence.
- Not settled here: the examples for allocator selection, jobs, reload and capture on the consumer. (Amended
  2026-10-12: settled below.)

Settled (2026-10-12, [session](../sessions/2026-10-12-diag-11a-examples.md)):
- The examples are routes of the same thin application, each in its own `example_*.cpp`, each a fresh process with a
  fresh `DiagnosticHost` started from a profile the package test authors from the installed investigation profile
  (watchdog window 50 ms) and policy (16 post events, a 1 s post window, a hitch threshold no frame reaches). Each is
  judged on every packaged binary and its bundles are read back through the application's own `inspect` route.
  Separate processes, because `watch_allocator` is one per host, `start()` is refused once the pool runs and the
  deliberate failure ends its process.
  The application now links `Cerid::memory`, `Cerid::jobs` and `Cerid::toml` besides `Cerid::perf`, the modules the
  examples name (the block above says `Cerid::perf`; that held before the examples).
- Allocator selection (`allocator <profile> <tlsf|diagnostic>`): the application chooses the allocator its owner gets,
  a TLSF heap over an arena it owns or crd-memory's `DiagnosticAllocator` over that heap (its records in a second,
  independent arena), and hands the owner `watch_allocator(chosen)`. Exhausting it captures an allocation-failure
  bundle whose trigger names the chosen allocator; the diagnostic choice also reports an overrun of one byte past an
  order (`redzone-overrun` at byte 1024), recorded by the application and kept in the bundle before the failure. The
  host is stopped before the allocator it watches is destroyed.
- Jobs (`jobs <profile>`): the host starts before the pool and gives it the profile's watchdog window
  (`configure_jobs`); the pool's work is checked against its known total and recorded from the workers; a job pinned to
  a main thread that neither pumps nor waits is reported as a hang (executor starved) and captured; the pool is shut
  down, joining the watchdog, before the host stops.
- Reload (`reload <profile> <limits.toml> <dir>`): the package has no resource manager, so the examples reload what an
  application of this family reloads. The diagnostic configuration through `apply()`: unchanged, an edited policy file
  applied live at a new generation, a changed watchdog window refused as RestartRequired with the field named, a new
  capture directory applied live. The application's own authored data (an order-limits table read with crd-toml)
  through its own reader, which consults the engine's I/O fault seam (`crd::consult_io_fault`, public in crd-core), so
  a seeded `failed-read` plan armed through `fault.arm` fails the reload, the last good generation is kept and the
  injected-fault capture lands in the new directory. Without fault seams `fault.*` is not served and every reload loads.
- Capture (`capture <profile>`): a frame loop feeds `host.frame()`; captures requested through the host's own service
  (`flight.trigger`, record authority) are written while the application runs, each with the frames before and after
  the request, and the directory keeps the newest `max_bundles`.
- What is shown where: the five example runs passed on `win-debug`, `win-shipping`, `win-shipping-profile`,
  `linux-gcc-debug` and `linux-gcc-shipping`; hosted, the Debug public-check presets run them in `crd-package-consumer`.

<a id="diag-11b"></a>
## DIAG.11b — cost, CI and fast investigation workflow

Measure the declared budgets on allocation-heavy, tiny-job, mixed I/O, authored rendering and offline workloads;
report p50/p95/p99, memory, drops, I/O and capture completion. Compare off/basic/full modes and independent external
sampling; use peer tools only on comparable features. Store measured boards at measurement time, including losses.
Extend dev.py/CI ownership and conclusion artifacts to diagnostic specimens and mode activation.

Acceptance: focused local plan selects affected tests and the relevant risk route; hosted Windows/Linux sanitizer,
race and short corpus/negative-control jobs have nonzero execution and usable artifacts. Longer fuzz/soak runs use
nightly/manual cadence. No blanket local sweep. Document symptom→command→artifact→next discriminating test and human-only publication.

Settled (2026-10-16, [session](../sessions/2026-10-16-diag-11b-overhead-harness.md),
[board](../bench/2026-10-16-diag-11b-overhead.md)):
- The event-site form: `CRD_FLIGHT_RECORD(recorder, code, severity, message, a, b)` (`crd/perf/flight_recorder.hpp`)
  evaluates its message and scalars only when a recorder runs and its detail keeps the event
  (`FlightRecorder::admit`, one atomic load, which counts a left-out event in `total_filtered()`); the recorder and
  severity expressions once. `record()` decides a left-out event before it reads the clock or claims a sequence.
  This is the modes contract's "disabled event sites must not evaluate expensive payload expressions", held by
  side-effect controls, and a Basic recorder no longer serializes informational sites on its ring (a no-wait case with
  the ring held). Application sites use the macro; a direct `record()` stays for code that already holds its payload.
  The installed application (`tests/support/package/diag-consumer`) writes every site in this form, formatting its
  message inside the message argument (`site_text`, which counts); `crd-package-consumer` runs its failure at the
  full and the basic detail and holds the messages formatted to the events kept (all forty orders at full; at basic
  the five refunds, with the other thirty-five left out unformatted and the bundle holding only the five).
  ([session](../sessions/2026-10-16-diag-11b-installed-sites.md))
- The modes as measured: off (no host, no recorder, profiler not started: runtime-disabled; in a binary built without
  profiling also compiled-out), basic (the `DiagnosticHost` with `assets/diag/overhead_basic.flight.toml`, Warning and
  above), full (`overhead_full.flight.toml`, every event, plus the profiler, the jobs adapter and the CEIR profiling
  bridge). The harness is `tests/support/diag-overhead` (`crd-diag-overhead`, one workload and mode per process, record
  `cerid-diag-overhead/1`); workloads `alloc` (a TLSF heap the host's allocator watch observes), `jobs` (2048 tiny jobs on a
  pool of `--threads`, the main thread included), `offline` (cook, load, compile and run an authored CEIR program),
  `io` (mixed I/O: 50 whole-file, range and `AsyncFile` reads a frame, 1.25 MiB, on a two-thread pool), `render`
  (the scene renderer draws 4096 instances through the authored `engine://frame/forward_csm` graph on a headless device)
  and `events` (a site microbenchmark, reported per site, never judged). Every frame has a known answer.
- What holds on every lane: the 18 `crd-diag-overhead-*` CTests run every workload in every mode with `--check`: off
  evaluates no payload, starts no diagnostic thread and acquires nothing; basic filters every site and frame event and
  records none; full records every one and evaluates each payload once; every requested capture is delivered and
  written; the lifetime ledger balances. `render-validation-core` (and `-sync` on Linux) runs `render` with device
  validation: the mode active on the device it draws with and no validation message; its cost is reported, never
  budgeted (2026-10-24, [board](../bench/2026-10-24-diag-11b-device-validation.md)).
- How a budget is judged: `scripts/diag-overhead.py` interleaves the arms across processes, pools nearest-rank
  percentiles, pairs per-repetition median ratios and takes a seeded bootstrap 95% interval; within (upper end at or
  below the budget), over (lower end above it) or unresolved (a loss, never a pass). Each budget is judged on the
  frame time and on the CPU per frame (every thread's CPU over the timed frames: `QueryProcessCycleTime` cycles on
  Windows, `CLOCK_PROCESS_CPUTIME_ID` nanoseconds on POSIX), the worse verdict deciding. Storage: the recorder's ring
  and the resident growth over off, against 32 MiB.
- The first boards (Windows win-shipping-profile against win-shipping; Linux linux-gcc-shipping and
  linux-gcc-relwithdebinfo under WSL2): storage within everywhere (152 KiB ring, at most 0.60 MiB growth); basic within
  on offline where resolved. Violated and open: the disabled mode on allocation-heavy frames (+37.7%, the allocator
  statistics `memory_stats.hpp` tracks in every profiling build, shown by experiment) and on offline (+6.6%; +4.6%
  without the statistics, unattributed); the basic mode on allocation-heavy frames (+4.0% to +5.7%, the allocator
  watch's forwarding; since 2026-10-24 the watch observes refusals instead and basic is within 2% there,
  [session](../sessions/2026-10-24-diag-11b-basic-allocator-watch.md)). Jobs (2026-10-24,
  [session](../sessions/2026-10-24-diag-11b-jobs-cpu-per-frame.md)): unresolved on every tuple with an eight-thread
  pool on sixteen logical processors (eight cores), where thread placement spreads one process's frame from another's
  by 7.6%; with a four-thread pool on eight cores, one logical processor each (2-3%), and 96 to 288 processes per arm,
  basic is within on all three tuples on the frame time and the CPU per frame, once left-out events are counted in
  cache-line stripes (one counter every thread wrote cost linux-gcc-relwithdebinfo's basic about 1.2 points), and the
  first 48 threads own a stripe and add without a locked instruction: that add was nearly all of a left-out site's cost
  (3.2 ns, now 1.0 ns), and on Windows it spaced the pool's contended dequeues so basic read below off (2026-10-24,
  [session](../sessions/2026-10-24-diag-11b-jobs-basic-below-off.md)); the
  disabled arm's timing stays inside the two-binary floor (within on one Windows board, unresolved on the next), and
  its instructions decide (below). Reported: full mode. Of its mechanisms against "no blocking or global locking", the jobs
  adapter's token mutex and the flight recorder's ring mutex per kept event are gone (2026-10-24); the CEIR bridge's
  per-instruction timing takes no lock, blocks nowhere and allocates nothing, and reads the cycle counter instead of
  the monotonic clock (2026-10-24,
  [session](../sessions/2026-10-24-diag-11b-ceir-bridge-cycle-counter.md)): offline full over off fell from +452% to
  +220% on win-shipping-profile. The rest is the price of exact parent-pause self-time, two counter reads per
  dispatch against a dispatch of a few nanoseconds. Full on jobs (2026-10-24,
  [session](../sessions/2026-10-24-diag-11b-jobs-full-attribution.md)): `register_thread` re-registers a thread of the
  live lifetime without the reader pin and the jobs adapter's tallies are striped, which took four-thread jobs full
  over off from +47% to +32% on win-shipping-profile and from +40% to +36% on linux-gcc-relwithdebinfo; the rest is
  attributed (payload formatting, the event slot write, the recording word, the adapter's clock reads and token
  claims).
- The flight recorder's record path takes no lock (2026-10-24,
  [session](../sessions/2026-10-24-diag-11b-flight-recorder-lock-free-record.md)): one recording word holds the total
  and the state; a record claims its sequence by compare-exchange only while Armed or Triggered, and the claim that
  fills the post window closes it in the same exchange; a trigger moves the word to Triggered in one exchange, so its
  sequence is the exact boundary. A slot's sequence is its release-stored publication stamp; collectors and a policy
  reload wait for every claimed slot. A record waits only when the ring laps a claim stalled before its publication,
  or while a reload swaps the ring.
- The focused local plan (2026-10-19, [contract](developer-workflow.md#risk-routes)): `dev.py plan` selects the
  affected targets and tests and prints each risk gate's route, the local commands and the hosted lanes.
- The investigation runbooks (2026-10-19, [runbooks](diagnostic-runbooks.md), ADR-0133 ID-6): one entry per symptom
  (symptom, command, artifact, read, next discriminating test, hosted lanes, the one adoption step, a working example,
  the cost, the owning children), human-only publication stated and refused, and every entry held to the tree by
  `crd-check-diag-runbooks` on every Ninja lane; every child owns an entry except DIAG.11c and DIAG.12, listed there.
- Source routes (2026-10-19, [contract](test-instruments.md#source-routes)): `crd-check-source-routes` judges every
  first-party source against what each configured tree compiles, so a test or specimen that no target builds fails
  the lane that should have built it; the conditions, outside routes and owned unrouted programs are named in
  `scripts/source-routes.json`.
- Allocator statistics on request (2026-10-19,
  [session](../sessions/2026-10-19-diag-11b-allocator-statistics-on-request.md)):
  a profiling build without `CRD_DEBUG` (win-shipping-profile) counts an allocator's operations only once a reader
  requests them (`IAllocator::request_statistics()`, which `crd::perf::register_allocator` calls) before the
  allocator's first allocation; unrequested, an operation costs one relaxed load. A `CRD_DEBUG` build counts every
  allocator from construction, unchanged. `MemoryStats::coverage()` (`crd/memory/statistics_coverage.hpp`) says what
  the counts cover (complete, not requested, too late, kept elsewhere by a decorator or composite, compiled out), and
  the profiler's snapshots, every `AllocatorRecord` (`stamps` bits 32-39), `memory.allocators` and the panel carry it,
  so a zero is never read as a measurement. The overhead harness holds its heap's statistics to what each build counts
  unasked in every mode, on every lane. Measured ([board](../bench/2026-10-19-diag-11b-allocator-statistics.md)): the
  disabled mode's allocation-heavy cost fell from +37.7% to +4.7% to +5.0%, still over 1%, and a gate-against-no-gate
  pair attributes the rest to the per-operation gate itself (no gate: +0.12%); the same board shows that comparing two
  separately compiled binaries has a layout floor of several percent (offline -11.0% between builds that do not touch
  its path), which the instruction board (below) does not have. Judged on realistic
  frames (2026-10-23, [session](../sessions/2026-10-23-diag-11b-realistic-disabled-cost.md)): an exact count of gate
  executions (1,008 per offline frame, none per jobs frame) times the pair's upper 0.58 ns per gate bounds the gate at
  0.18% of an offline frame, within 1%; the `alloc` microbenchmark stays the reported worst case.
- The disabled mode on instructions per frame (2026-10-24,
  [session](../sessions/2026-10-24-diag-11b-instruction-counts.md)): `diag-overhead.py instructions` runs both
  binaries' off mode under Valgrind's cachegrind at two frame counts (the difference per frame cancels setup; repeats
  span the interval), judging the instructions executed, which the linker's layout does not change, and reporting the
  simulated cache's misses. On linux-gcc-shipping against a scratch configuration of it with profiling on: offline
  +0.08% and jobs +0.00% (four threads -0.23% .. +0.23%), within 1%, the misses within a percent; alloc +5.55%, the
  reported worst case. The extra work is the statistics gate (7.8 to 8.5 instructions per execution), so offline's
  disabled timing residual on Windows (+4.6%; -11.0% to +0.2% across rebuilds) is layout, not work. The same pair's
  timing boards are within on both workloads, frame time and CPU per frame. Windows has no unprivileged instruction
  counter here: its disabled verdicts rest on the exact count times cost; an MSVC instruction count is DIAG.11c open
  qualification.
- The default allocator's exact sizes (2026-10-19,
  [session](../sessions/2026-10-19-diag-11b-malloc-allocator-bytes.md)): `MallocAllocator` aligns inside one C-library
  `malloc` and keeps a 16-byte header below each block (the request, and the offset to the C-library block with
  log2 of the alignment), so `allocation_size()` is the request on every platform and its statistics count the same
  bytes at allocation and at free. Before, Windows answered 0 (the CRT cannot size an `_aligned_malloc` block without
  its alignment) and the bytes in use never fell; glibc answered the usable size and wrapped them. Under ASan the
  header and the alignment padding are poisoned and read through `CRD_MEM_ASAN_UNCHECKED` loads, so a run before or
  past a block is reported as before; a header that fails its check is a named fatal at the free in every build (a
  check, not ownership: `owns()` stays true for every pointer).
- Mixed I/O (2026-10-24, [session](../sessions/2026-10-24-diag-11b-mixed-io-workload.md)): `io` reads files its setup
  writes through the three reads the I/O fault seam consults, hashing every byte. Disabled within 1% on Windows (frame
  time and CPU per frame) and on Linux by instructions (cachegrind) and timing; basic within 2% on all three tuples;
  full +0.1% to +2.5%, reported. Its pool is fixed at two threads: with four, the Linux reference host's frames
  alternate between about 210 and 400 µs in every arm, compiled-out included (Windows is flat). Since the readers
  stopped zeroing the bytes they read into ([session](../sessions/2026-10-24-diag-11b-file-reads-zero-pass.md)) the
  Linux frame is 172 µs, disabled and basic within, full +3.2%.
- Authored rendering (2026-10-24, [session](../sessions/2026-10-24-diag-11b-authored-rendering-workload.md)): `render`
  is the sandbox's frame path without its window: 4096 instances (1024 behind the camera, 64 moved a frame) through
  `engine://frame/forward_csm` with its cascades into an offscreen target on a headless device, D3D12 on Windows and
  Vulkan elsewhere, its heap under the host's allocator watch in basic and full. The first frame must draw and cull
  exact counts, run its passes on the device and cover the grid's pixels. A workload's process-lifetime caches (the
  Vulkan backend's shaderc) fill before the ledger's window (`Workload::warm`). Disabled within 1% and basic within 2%
  on the hardware D3D12 tuple (192 processes) and on llvmpipe (both Linux tuples); full +0.1% to +1.5%, reported.
- The controlled-interleaving twin (2026-10-24, [session](../sessions/2026-10-24-diag-11b-schedcheck-ctests.md)):
  `crd-jobs-schedcheck-tests` is in the default build and its eight cases (`^schedcheck: `, two of them broken
  variants that must be found) are CTests on every lane, ASan and TSan included; the twin library stays out of ALL.
  Cost: crd-jobs' units again, the test unit and two links a lane; the cases take 0.8 s (win-debug) to 10.4 s
  (linux-gcc-asan). The race runbook's `ctest -R "^schedcheck: "` makes `crd-check-diag-runbooks` fail a tree that
  does not register them.
- Modes and specimens in the conclusion artifact (2026-10-24, [session](../sessions/2026-10-24-diag-11b-conclusion-modes-labels.md)):
  every lane's `conclusion.json` names the diagnostic modes its binaries are built with (`modes`, from the CMake
  cache: ASan, UBSan, TSan, libFuzzer, coverage, asserts, profiling, clang-tidy) and counts the executed tests per
  label (`results.labels`): `diag-fatal` (every `crd_diag_fatal_tests` mode), `diag-race` (the schedcheck cases),
  `diag-overhead` and `fuzz`; the job summary shows both, so a lane that ran none reads 0. `dev.py plan` owns a
  specimen's CTests through its artifact: `array.hpp` selects crd-containers' nine fatal modes.
- The sanitizers' cost (2026-10-24, [board](../bench/2026-10-24-diag-11b-sanitizer-cost.md)): per workload the off
  arm of each sanitizer lane's harness against its reference (ASan 1.5 to 7.1 times, TSan against GCC Debug 1.9 to
  8.9 times), per lane `ci-evidence.py compare` on two hosted evidence bundles (a median test 2.2 times on win-asan,
  2.7 on linux-gcc-asan, at most 2.6 on linux-clang-tsan). The board found, and the fiber annotations no longer have,
  a fake stack mapped per job under ASan and a TSan frame left per job on a fiber's shadow stack.
- Not settled here: the repairs and the measurement gaps above; independent external sampling and peer tools; the
  cost of device validation and of the generation checks, recording and probes, and the hosted audit's per-label
  counts on a run of the current head.

<a id="diag-11c"></a>
## DIAG.11c — native qualification and portable capability boundary

Qualify actual Windows/Linux CPU/GPU/driver/build tuples and unsupported-mode handling, including symbols, optimized
execution and current providers. Validate portable schemas and host adapters contain no x64/native-filesystem-only
assumption in public contracts. Add later MAC.DIAG/WEB.DIAG contract hooks without claiming their execution passed.
Native ARM64/fiber evidence belongs to JOBS-PORT and its consumers, and cannot be inferred from x64 success.

Acceptance: one complete native diagnostic workflow per required available tuple, plus missing tools/hardware documented
as open qualification, not pass. A software Vulkan adapter qualifies that adapter only. Do not procure/register new
hardware/services. DIAG cannot close on required Windows/Linux evidence that is unavailable; later-platform rows stay explicit.

Settled (2026-10-24, [session](../sessions/2026-10-24-diag-11c-doctor-modes.md)):
- The doctor (`run_doctor`, `diag.capabilities`) reads every mode from this build or host: the tuple and the
  instruments from `current_build_identity()`; `memory-check` (ASan) and `race-check` (TSan) each need their own build
  and, absent, name its CMake switch; `crash-capture` follows the platform's crash backend and, absent, names
  MAC.DIAG and WEB.DIAG; `controlled-replay` and `authored-provenance` follow the commands registered with the
  service (`DoctorLive::commands`). GPU validation and device faults are the GPU device's to report.
- `doctor:` cases hold the instrument modes to `lane_promise()` on every lane and the upper-module modes to the
  registered commands; `diag commands:` holds `diag.capabilities` to the same.
- The native workflow (2026-10-24, [session](../sessions/2026-10-24-diag-11c-native-workflow.md)): the
  `[diag-workflow]` cases, CTest label `diag-workflow` (`ctest -L diag-workflow`), run one route per tuple: the doctor
  names the tuple; an application fails on purpose under its profile and its bundle carries this build's identity,
  the executable's module identity (PDB or GNU build id) and the forty events `bundle.inspect` decodes; a real crash
  is captured (a minidump whose every module resolves to a debug-file path and whose wrong symbols are refused on
  Windows, a signal record on Linux); a device fault (DX12 on WARP) and a Vulkan loss run the GPU providers. Every
  lane runs them, so each `conclusion.json` counts them beside its toolchain, modes and adapters. Under ASan the
  sanitizer owns the crash, so the capture steps qualify on the same tuple's other lanes.
- Open qualification, named, not passed: Vulkan on the hosted Windows runners (no Vulkan device; qualified on this
  host's hardware adapter), a native Linux hardware GPU (lavapipe qualifies that adapter only), ARM64 (JOBS-PORT),
  the hardware DX12 page fault (user-only h2) and MAC.DIAG/WEB.DIAG.
- The portable boundary (2026-10-24, [session](../sessions/2026-10-24-diag-11c-portable-boundary.md)): every
  on-disk schema has fixed-width fields and a byte-buffer writer and reader beside any file route, and either writes
  little-endian bytes (flight record, build identity) or asserts a little-endian host where it copies host bytes
  (bundle, manifest, symbol index, CPROF). The crash adapter takes and reports UTF-8 paths. A diag command path
  refuses a component ending in `.` or naming a Windows device on every host. Named for later platforms: wasm32
  (`platform.hpp` refuses it; JOBS-PORT.3, WEB.DIAG), 64-bit macOS thread ids against `u32` `faulting_tid` and
  `current_thread_id()` and a Mach-O UUID symbol kind (an older reader clamps an unknown kind to `None`) (MAC.DIAG),
  and the file adapters `crash::install`, `write_bundle_atomic`, `save_capture_to_file` (WEB.DIAG takes the buffer
  routes).

<a id="diag-12"></a>
## DIAG.12 — independent end-to-end close audit

Re-read every child and DG finding against final source, public consumers, artifacts and CI. Use the same app to
demonstrate allocator UAF, job race, wait failure, fatal crash, authored-program error and GPU hazard in isolated
specimens, then demonstrate repaired versions. Confirm capture import/inspection, source attribution and privacy.
Remove obsolete diagnostic scaffolding/examples and duplicated services; retain useful external tooling recipes.

Acceptance: all claimed detection classes pass both faulty and valid controls; all required Windows/Linux evidence
is tied to the published code; documentation/recipes/bench/indexes are current; every residual capability limitation
has an explicit owner. DIAG closes only after every child, never because code builds or a dashboard looks complete.

Settled (2026-10-24, [session](../sessions/2026-10-24-diag-12-same-application.md)):
- The same application is `crd-diag-host-specimen` (DIAG.11a), run as a bounded child per mode, `<mode> [repaired]`:
  `invariant` and `assert`, `use-after-free` (the `DiagnosticAllocator` quarantine; the event before the fatal
  invariant carries the allocation and free PCs, which the bundle's module list places in the application), `wait`
  (a wait cycle the host's watchdog window reports as a `Hang` bundle) and `race`. Each faulty run is caught by the
  engine's own detector on every lane, except the race: only ThreadSanitizer catches it, and elsewhere it is
  InstrumentAbsent, judged against the lane's promise. Every repaired run exits 0 with no bundle and no dump.
- A crash (`crash`, a wild write) is caught by the crash handler the host installed: one dump or signal record, no
  bundle. The capture's module list goes through the bundle import route (`build_symbol_index_from_minidump`, or
  `parse_crash_record_modules` for the Linux record, whose module lines carry the PT_LOAD span) and places the
  faulting instruction (the dump's exception address, the record's `rip`) in the application's module. Under ASan the
  sanitizer owns the fault ([session](../sessions/2026-10-24-diag-12-application-crash.md),
  [import](../sessions/2026-10-24-diag-12-crash-record-import.md)).
- One walk reads a loaded ELF object's PT_LOAD span and GNU build-id: `crd::crash::read_elf_image`, used by the crash
  record's module lines and `enumerate_loaded_modules` ([session](../sessions/2026-10-24-diag-12-one-elf-walk.md)).
- An authored-program error (`program`) is the CEIR run's own fault: the application cooks, loads, compiles and runs
  the authored `orders.ceir` beside its profile, and reports a run fault as a `Manual` trigger (code 0x0713) whose
  source is the faulting op's authored file and line and whose message is its rendered provenance; `stop()` writes the
  bundle ([session](../sessions/2026-10-24-diag-12-authored-program.md)).
- A GPU hazard (`gpu`) is a missing barrier between two overlapping copies the application records into one Vulkan
  buffer through the public `ComputeRecorder`, inside a debug label carrying its own Pass identity. Synchronization
  validation reports the write-after-write; the application reports it as a `Manual` trigger (code 0x0714) naming the
  buffer's and the pass's identities. A host without a Vulkan device, the layer or synchronization validation says so
  and exits 10, never clean; only `no-vulkan-device` on a Windows host is accepted (the hosted runners have no Vulkan
  device). DX12 has no route for this class: its compute context inserts every transition and it has no
  synchronization validation; its correlated Core hazards are DIAG.7a's
  ([session](../sessions/2026-10-24-diag-12-gpu-hazard.md)).
- Privacy (ID-8): every bundle the application leaves, and the symbol section imported from its crash capture, holds
  neither the application's directory nor the one holding its profile, captures and authored program.
  `serialize_symbol_index` packs each module and debug file as its file name, whichever route supplied the path (the
  live walk, a dump, the record); the authored program is registered by its file name; the replay recipe is
  placeholders, never argv. The dump and the Linux record are raw evidence: they keep full paths, registers and
  memory, the case checks that they hold the application's directory, and nothing declares them redacted (DIAG.5d).
  An engine site's trigger `file` is the build's `__FILE__` (DIAG.9b), a path the binary already carries. The race
  leaves no capture; ThreadSanitizer's report is the tool's own stderr
  ([session](../sessions/2026-10-24-diag-12-privacy.md)).
- The schema's redaction class is `crd::RedactionClass` (`crd/core/redaction.hpp`, shared with DIAG.8b's inspected
  values). A flight event, a flight trigger and a `DiagnosticEvent` carry it; a Restricted one keeps its code,
  severity, source and scalars and no text: the recorder never copies the text, so no bundle, `bundle.inspect`,
  `flight.status` or summary can carry it, and `make_event` keeps no message. The flight record holds the class in
  the event's flags byte (bit 1; the clip flag is bit 0) and the trigger's in the record flags (bit 3), and refuses a
  restricted record with text. A CPROF capture's names are instrument identifiers the program registers and are
  Public by kind. The application's last order names its customer and is Restricted; no bundle it leaves holds the
  name. The manifest has no string fields, so version 1 defines no flag bit and reports every set one
  ([session](../sessions/2026-10-24-diag-12-redaction-class.md)).
- Duplicated services: the DIAG.2b `DiagnosticRecorder` had no production consumer and is removed; the doctor reports
  no `emergency-recording` mode, and the [DIAG.2b](#diag-2b) section names each clause's carrier. Every specimen target
  has a consuming test ([session](../sessions/2026-10-24-diag-12-one-recorder.md)).
- Unwired: DIAG.6c(i)'s `FramePacingAnalyzer` reaches the user through the capture's export. `export_perfetto_json`
  (so `cprof_export`) writes a `pacing` block in the document's metadata: the frame interval's mean (throughput),
  p50, p99 and maximum (pacing), the bucket width and the mean work per frame; `cprof_export` prints the same numbers.
  A capture records no input or present time, so the block says `not recorded`; UX-QUAL.2 owns recording them. The
  analyzer reads untrusted capture times through `span_ns` and saturating sums
  ([session](../sessions/2026-10-24-diag-12-frame-pacing-route.md)).
- The log mutex (DIAG.2b, DIAG.5a/5b): the crash-capture specimen's `logger_lock` mode faults inside a crd::log
  sink's `write`, holding the logger's sink mutex and the stdio stream lock. The handler takes neither, so the dump
  or record is still written; a handler that logged would time out
  ([re-read](../sessions/2026-10-25-diag-12-re-read.md)).
- A sanitizer catch names the specimen's site: `Expectation::expected_site` is the function its report must name.
  A report that does not is `MissingSymbolizer` when none of its frames is symbolized (DIAG.0's missing symbolizer,
  seeded by `asan-symbolizer-off`) and `Unexpected` when frames are. Every residual limitation of the children has an
  owner row, named in each child's ROADMAP row, or is DIAG.12's own remaining work
  ([session](../sessions/2026-10-25-diag-12-owner-rows.md)).
- The diag asset reader (DIAG.10b): `read_diag_asset_file` consults the I/O fault seam as a `ReadWhole` before it
  opens the file, as crd-platform's whole-file reads do, so a failed, short or cancelled read is `Failed` and a
  `failed-read` plan fails a host profile, flight policy or fault-plan read
  ([session](../sessions/2026-10-25-diag-10b-asset-read-seam.md)).

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
