# System qualification and agent close-out contract

<!-- doc-role: contract -->
> Acceptance contract; live work and findings belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

This extends the [renderer/UI contract](renderer-ui-execution-contract.md) across Cerid's application substrate.
The [whole-system review](../research/2026-09-12-cerid-whole-system-review.md) supplies evidence and rationale.
It does not certify existing code. Apply the relevant sections to every slice; record why a section is inapplicable.
New architecture choices follow [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md).

[DIAG](runtime-diagnostics.md), ordered immediately after REPO.DEV by
[ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md), defines the runtime diagnostic foundation.
Every future module/host must integrate its lifetime, source identity, failure/replay and bounded telemetry contracts.
Qualify detectors with intentionally faulty controls; a dashboard, clean retry or missing report is not safety evidence.

## Contract before implementation

Apply [ordered execution](../ROADMAP.md#strict-sequential-execution): start with the earliest unfinished work unless
the user explicitly directs a future slice. Finish available work and documents; retain CI-only waits as Needs CI
while continuing the next available work. No gate disappears and no missing evidence is accepted as Done.

Each slice's linked design states its complete behaviour, public owner, dependencies, reuse search, authorable assets,
source/cooked/schema migration, deletion targets, failure/recovery cases, platforms, workload and oracle. Split work
into children in the master table when it cannot be reviewed as one change. Keep inherited requirements reachable.
No unlabeled "future" in prose may silently substitute for an owning row. Design choices and runtime defects are
different findings; neither is closed by making a list.

Give every property a type, physical dimension where applicable, default, bounds, invalid-input response and change
notification. For commands include identity, version, effects, capabilities, preconditions, transaction boundary,
idempotency/retry, cancellation, progress and structured result. Annotation/codegen, CHIR and graph producers must emit
the same reflection/command schemas; generated views are compared with their producers, never independently edited.

## Execution, ownership and recovery

- Author → validate → cook → install → execute → edit/shadow → reload → undo/reopen is one production path. Prove
  replacement without engine recompilation and remove superseded shipping algorithm builders. Native providers
  implement declared capabilities; an asset cannot acquire arbitrary filesystem/network/device authority by naming one.
- Identify ownership for every allocation, handle, callback, task and device submission. State which allocator frees it,
  which thread/fiber may access it, and which completion event ends its lifetime. A frame number is not GPU completion.
  Reload keeps code, descriptors, captures and data alive until all users retire. Failed migration preserves the last
  working generation and document; test cancellation during every migration stage.
- Fiber migration does not preserve worker-local scratch ownership. Suspended tasks retain their own arena or an
  explicit pinned owner. Test nested work, single worker, pool exhaustion, shutdown with outstanding tasks, re-entry,
  callback deregistration, priority pressure and cancellation races. Observe actual pooled execution; a parallel API
  silently taking a sequential path does not establish integration.
- Ordinary interactive tasks may yield; real-time audio and bounded control tasks have explicit no-allocation,
  no-blocking/no-lock policies and bounded work. Shared jobs infrastructure does not imply one undifferentiated queue.
  Admission, reserved capacity, priority inversion and deadline misses need measured stress gates.
- Exercise empty/large/malformed input, overflow in sizes/strides, NaN/Inf, OOM, device loss, queue pressure, disk full,
  partial writes, missing dependencies, permission failure and unplug/suspend. Restore a valid state or return a typed
  failure. Silent truncation, stale cache reuse and half-installed generations are failures.

## Cross-platform and hardware evidence

A capability entry is a tuple: OS/version, CPU architecture/features, API/provider/version, adapter/driver, feature
tier, build/compiler, presentation mode, asset versions and workload. Do not claim a Cartesian product from isolated
successful cells. Maintain measured results as dated evidence; the corresponding master row owns qualification state.

Windows/Linux first includes Vulkan on both and DX12 on Windows where supported, native window/input/audio behaviour,
headless use and clean-machine export. Select representative Intel/AMD/NVIDIA GPU classes and x64 CPU feature tiers;
integrated/discrete, UMA/discrete memory, subgroup/descriptor limits and low-VRAM cases matter. Test an unsupported
capability's declared fallback or explicit rejection. ARM64, macOS/Metal, browser/WebGPU, mobile and other targets retain
their own release rows. WSL helps verification but does not prove a native Linux display/compositor/driver path.

Browser qualification includes worker/event-loop scheduling, threaded and single-thread deployments, isolation headers,
asynchronous filesystem/fetch, persistence quotas, offline reopen, GPU loss, tab suspension, input/IME/accessibility,
audio activation and secure transport. No native DLL, raw socket or x64-fiber assumption may leak into its public API.

Use one primary local configuration, relevant guards and affected consumers, adding local lanes only for a
discriminating risk or reproduced failure ([BUILDING](../BUILDING.md)). Broad compiler/hardware matrices run in CI or on
recorded target hardware. If required hardware is unavailable, keep that gate open and state exactly what evidence is
missing. Platform-neutral design is mandatory now; later-target release evidence remains explicit rather than invented.

## Correctness, performance and visual quality

Separate bit-exact contracts, bounded numerical error and perceptual output. Floating-point representation agreement is
not universal physical correctness. Match peers on algorithm/workload, precision, solver tolerance, time step, features,
threads, hardware, input distribution and timing boundaries. Report warm/cold paths, cook/startup, steady state, tails,
memory/VRAM, transfers, cancellation and energy/thermal effects where material. Keep all peers and losses in the board;
predeclare budgets and significance before optimizing. A throughput gain cannot excuse a quality regression.

Full-victory tasks require measured wins across their declared comparable board; ties/losses remain open. A broad
"beats every engine" claim without a specified workload is invalid. Physics uses analytic/invariant/oracle evidence in
addition to matched peer scenes. Scientific solvers include conditioning, convergence, uncertainty and unit/frame/time
tests. Neither a faster inaccurate answer nor matching another engine's bug meets the contract.

Renderer qualification covers spatial and temporal error, disocclusion, convergence/bias, material/light transport,
HDR/color/alpha, motion blur, noise, low-sample behaviour and recovery on real scenes. UI qualification adds readable
typography, coherent tokens/states, dense-layout hierarchy, keyboard/IME/assistive technology, localization, RTL,
reduced motion and high contrast. Goldens are reviewed alongside end-to-end tasks and measured input-to-present latency;
one attractive screenshot or a fast rectangle benchmark cannot qualify the product.

## Data, security and agent access

Treat files, model weights, shader/IR programs, plugins and packets as inputs with declared trust levels. Validate
lengths, schema, recursion, references and resource budgets before allocating/executing. Content hashes detect identity;
they do not authenticate publishers. Keep trusted-native and isolated-untrusted extension modes distinct. Use reviewed
cryptographic protocols and maintained implementations behind owned interfaces; do not invent crypto for peer-crush.

Remote commands authorize principal, project, object, operation and budget before dispatch. Constrain filesystem roots,
network destinations and device access. Separate read-only inspection, reversible edits, code/provider installation,
publishing and physical actuation. Tool descriptions/project content never expand authority. Validate model-produced
arguments using the same rules as GUI inputs; record provenance, actor and resulting changes. Agent experiments run in
bounded workspaces, can be cancelled, and must be replayable from recorded inputs/results under a declared tier.

Networking acceptance covers authenticated admission, replay/expiry, bounded reassembly and decompression, anti-
amplification, congestion/backpressure, connection/tenant quotas, abuse under established identities and recovery.
Reject invalid input before application deserialization or expensive work; header/authentication checks still consume
bounded resources. Publish offered-load, packet/byte rates, CPU/memory and legitimate-client latency under attack.
DDoS resistance also needs deployment capacity/upstream mitigation; no "DDoS-proof" claim is permitted.

Durable project collaboration, live game simulation and audio timing use separate semantic contracts. Test concurrent
delete/edit/undo, schema migration, offline reconnect, retries, stale authorization, failover and corrupt history.
Convergence alone does not guarantee a valid CAD feature graph or preserve the user's intended edit.

## Close-out for every kind of task

1. Record exact scope, observations and verification in a dated session, including partial/failed/investigation-only work.
   Record commands, build/revision or working-tree identity, platform, matched test counts, exit status and artifact paths.
2. Save full measured peer boards at measurement time; new implemented research gets a parameters-first recipe. Pure
   planning does not invent a benchmark, implementation recipe, runtime result or advisor review.
3. Update the owning master row, children, evidence and dependencies. Done requires the entire inherited contract.
   An unresolved verified defect blocks completion. A source concern remains Review until investigated.
4. Inspect affected source/API examples, design/ADR, systems entry, build instructions, README, AGENTS, PRINCIPLES,
   SANITY, MEMORY and context. Correct facts/routes that changed; do not rewrite correct documents or historical boards
   merely to change their date. Add new documents to the appropriate index and row. Remove redundancy only after unique
   content has a verified preserved destination.
5. Run `python scripts/check-master-plan.py`, inspect the scoped diff and leave the next exact master-row pointer.
   Parent closure requires all child evidence; never tick it from test count alone. The user commits/pushes.

An interrupted handoff gives a reproducible resume action and the open gate. The invariant is **no changed fact left
stale**, not an uncontrolled rewrite of the entire documentation corpus after every code edit.
