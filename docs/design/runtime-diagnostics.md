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
  crd-sandbox builds the service from start-up flags only (`--diag-grant`, default `read`; `--diag-root`), serves the
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
- The flight recorder is `crd::perf::FlightRecorder` (`crd/perf/flight_recorder.hpp`), separate from the DIAG.2b
  `DiagnosticRecorder`: its events are fixed 152-byte records (code, severity, two scalars, a message clipped to 111
  bytes with the clip flagged), so recording and triggering never allocate. Codes 0x0900..0x09FF belong to it.
- Bounds are an authored, versioned `FlightPolicy` (capacity at most 65536 events, about 9.5 MiB; `pre_events`,
  `post_events`, `post_window_ns` at most 60 s, a trigger mask, `hitch_threshold_ns`). `pre_events + post_events <=
  capacity`, so the post window can never overwrite the pre window it keeps. A rejected candidate (or a reload while a
  capture is pending, or a generation that would wrap) leaves the active policy, its generation and the ring as they
  were. Defaults: 1024 events (152 KiB), 768 before, 256 after, 2 s, every trigger, 50 ms hitch.
- A trigger takes only its own mutex, never the ring's. The first trigger opens the post window; any trigger while a
  capture is pending is suppressed and counted with its kind, never replacing it. The window closes when
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
  path the ring is taken with a bounded try-lock; if it stays held, the capture carries the trigger alone and says
  `ring_unavailable`. Building a capture allocates, so the fatal path is an ordinary handler call, never a signal
  handler: a hard signal crash still leaves only the DIAG.2b emergency record and the DIAG.5a/5b crash record.
- The capture is bundle section `FlightRecord` (tag 6), a versioned little-endian payload ('CFR1') whose decoder
  bounds every length by its fixed limit and by the payload, checks the loss metadata against the event list and
  refuses trailing bytes; `write_flight_bundle` writes it beside a Manifest. `flight_capture_summary_json` is the
  one-line summary.
- Trigger sources (`crd/perf/flight_triggers.hpp`) sit on seams that already existed and change none of them:
  assertions (`crd::set_assert_handler`, chained to the previous handler; when the assert path will terminate,
  headless with no platform handler, the window is closed Fatal and delivered first), fatal invariants (new
  `get_fatal_invariant_handler` and `default_fatal_invariant`, chained the same way), queue starvation and hangs
  (`flight_on_starvation` / `flight_on_hang` for the jobs watchdog's handlers), frame hitches (`FlightFrameWatch`,
  fed by the host's frame boundaries), allocation failure (`FlightAllocatorWatch`, an `IAllocator` decorator: a failed
  `try_allocate` triggers; a failed `allocate` triggers before the backing allocator's own out-of-memory path, whose
  `CRD_FATAL` then closes the window through the assert trigger) and resource pressure (`FlightPressurePolicy`, an
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
- The resource manager allocates through `FaultAllocator` over `FlightAllocatorWatch` over the arena: an injected
  failure is one injected-fault capture (the watch never sees the injected null, so nothing is suppressed), and a real
  failure of the arena an allocation-failure capture.
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
    `PERMISSION_DENIED`. Unverified until an elevated recording runs.
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
  shrinks the recorded failing log to exactly `{test.reacquired, fp.finalizing}` with no race seen.
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
  on schedule-script reproductions (the schedcheck twin runs in no hosted job); the profiler's scope trace as a
  third instrumented level (a process singleton, compiled out on the perf-off lane).

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
  explanation.
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
- The commands' file readers (`bundle.inspect`, the diag asset reader, crd-ceir-cook's bounded file reader) open files
  directly, outside crd-core's I/O fault seam, so neither fault injection nor this target observes which paths they
  open; the target's root does not exist, so they open nothing.
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
