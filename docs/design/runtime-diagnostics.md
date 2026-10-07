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

Settled policies (2026-10-06, [session](../sessions/2026-10-06-diag-3a-allocator-boundaries.md)):
- **Wrong-allocator free.** Pool, growable pool, TLSF, growable TLSF and virtual memory, and the wrappers that route to
  them, refuse in every build a pointer they did not hand out, after an assert where asserts are compiled in. Arenas
  (linear, stack, growable linear) ignore every free. The diagnostic decorator reports `UnknownPointer`.
  `MallocAllocator` cannot tell, because `owns()` is true for every pointer; wrap it in the decorator to diagnose.
  TLSF also refuses a misaligned pointer and an immediate double free. Interior pointers that land on a valid header,
  and stale frees after reuse, need the decorator or DIAG.3e generations.
- **Partial construction.** An owning arena whose parent refuses its backing memory is out of memory: `CRD_FATAL`
  naming the arena, in every build. A growable pool whose parent refuses a page returns nullptr from `try_allocate`
  and stays usable; its `allocate` is fatal. `construct`/`construct_array` return nullptr for a refused allocation or
  an overflowing count, and undo a throwing constructor (built objects destroyed in reverse, storage returned).
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
  carries a Cerid Pass identity. This is the DX12 pass-to-fault route of DIAG.7a; the removal bundle is version 2.

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
  work ran unchecked.
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
  request nor an authored asset can raise it. Each command declares exactly one class.
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
