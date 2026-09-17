# DIAG.6b census — CPU/job/GPU/I/O correlation, timestamp bridge, clock uncertainty (DG11)

<!-- doc-role: historical -->

Row 078 ([DIAG.6b](../ROADMAP.md#slice-diag.6b)) census tick — the same opening move as DIAG.6a's (a): read the
contract, census the four subsystems against it, write a clause→status skeleton table and a numbered sub-unit plan.
**No implementation this tick.** Row 078 stays **Open**; the next tick begins the first implementable sub-unit after a
second advisor pass with this census in hand.

## Acceptance (verbatim, from [runtime-diagnostics.md#diag-6b](../design/runtime-diagnostics.md#diag-6b))

> Connect real frame-graph timestamps to crd-perf and model CPU scopes, logical tasks, queue wait, execution, GPU
> submissions and transfers separately. Support multiple queues/devices and disjoint/unsupported clocks. Add calibrated
> clock samples/uncertainty where supported, otherwise retain separate tracks. Do not introduce submit-and-wait stalls
> merely to make profiling convenient. Correct retired rhi examples and replace disconnected diagnostic assembly.
>
> Acceptance: an existing authored render/compute workload has linked CPU/task/pass/resource identities on DX12 and
> Vulkan, including delayed query resolve, ring reuse, timestamps unavailable, wrap and device loss. Compare total
> timing to an independent capture; do not subtract unrelated CPU/GPU timestamps or infer exact GPU failure location.

**DG11 (from [DIAG.0 capability census](2026-09-14-diag-0-capability-census.md) and
[the research map](../research/2026-09-14-diagnostics-and-instrumentation.md)):** "disconnected GPU traces" —
`IFrameGraph` has real DX12/Vulkan pass timings used by the sandbox, but only a **test mock** implements
`IProfilerGpuBackend`, and its public example names the **retired** rhi module. → DIAG.6b, DIAG.7a.

## What exists today (censused)

- **GPU bridge surface** ([gpu_scope.hpp](../../engine/foundation/perf/include/crd/perf/gpu_scope.hpp),
  [gpu_scope.cpp](../../engine/foundation/perf/src/gpu_scope.cpp)): `IProfilerGpuBackend` interface, `GpuSpanHandle`
  (opaque; Vulkan = VkQueryPool slot), `ResolvedGpuSpan` (GPU-tick pair + NameId), `set_gpu_backend`,
  `resolve_gpu_frames`, `emit_gpu_sample` (converts a resolved span to a `Category::Gpu` Sample on the single "gpu"
  track, `g_gpu_thread_index`). **Only a test mock** ([test_gpu_scope.cpp](../../tests/foundation/perf/test_gpu_scope.cpp))
  implements the interface — no shipping DX12/Vulkan backend.
- **Frame graph** ([frame_graph.hpp](../../engine/gpu/gpu-context/include/crd/gpu/frame_graph.hpp)): `FgQueue
  {Graphics, Async}`, `FgPassKind` (Raster/Compute/Present/"moves pixels" copy-blit-resolve), per-pass queue requests,
  async-compute-queue seam. Real pass timings are produced on the sandbox path but are **not exposed to crd-perf**.
- **Jobs** ([jobs_adapter.cpp](../../engine/foundation/perf/src/jobs_adapter.cpp)): one `Sample` per job, fiber-parked
  `BeginToken`, migration captured via `begin_thread`/`end_thread` and `fiber_id`. Task identity today = interned name +
  fiber_id; no explicit logical-task/queue-wait vs execution split.
- **Sample wire format** ([sample.hpp](../../engine/foundation/perf/include/crd/perf/sample.hpp), 32 B): `begin_ns`/
  `end_ns` (MonotonicClock-relative ns), `name_id`, `color_rgba`, `begin_thread`, `end_thread`, `depth`, `category`,
  `fiber_id`. **No queue id, device id, timestamp-domain tag, or clock-uncertainty field.** GPU spans are already
  converted to CPU-ns at resolve, so a CPU↔GPU offset is applied somewhere in the (mock) backend but **no uncertainty
  is carried**.
- **"I/O" here = GPU transfers, NOT a filesystem layer.** The 6b contract mentions no file/disk/stream/asset-load
  (grepped: none); the acceptance's own list is "CPU scopes, logical tasks, queue wait, execution, GPU submissions and
  **transfers**." So the row title's "I/O" is shorthand for transfer/copy traffic — the frame graph's `FgPassKind`
  "moves pixels" (copy/blit/resolve) plus the async/copy queue. There is deliberately no separate filesystem-I/O layer
  in scope; correlating transfers is a track, not a new subsystem.
- **Test lanes**: `CMakePresets.json` has **no GPU/DX12/Vulkan lane** — every real-device proof is env-gated on this box.

## Clause → status skeleton (the (final) acceptance table, built now)

| Acceptance clause | What exists | Defect / gap | Proposed sub-unit |
| --- | --- | --- | --- |
| Connect real frame-graph timestamps to crd-perf | mock backend + `emit_gpu_sample` path | frame-graph pass timings not bridged; `resolve_gpu_frames` **not** called from `frame_mark` (header claims it is) | (e) bridge + wire |
| Model CPU/task/queue-wait/execution/GPU-submit/transfer separately | `Category::Gpu`, jobs adapter | no queue-wait vs execution split, no transfer category, single gpu track | (c) tracks/categories |
| Multiple queues/devices | `FgQueue` in frame graph; single `g_gpu_thread_index` | not carried into perf; Sample has no queue/device id | (b) wire format + (c) tracks |
| Disjoint/unsupported clocks; calibrated samples/uncertainty else separate tracks | GPU spans converted to CPU-ns | no calibration record, no uncertainty field, no "unsupported → separate track" fallback | (b) wire format + (d) calibration |
| No submit-and-wait stalls for profiling | async `resolve_gpu_frames` | design constraint for (e)/(f) | constraint, all sub-units |
| Correct retired rhi examples; replace disconnected assembly | example names `crd-rhi-vulkan` (retired) | stale example; `resolve_gpu_frames` unwired | (g) example fix + (e) wire |
| Linked CPU/task/pass/resource identities on DX12+Vulkan | name+fiber identity | no pass/resource identity; no real backend | (b) identity fields + (i) backends **[env-gated]** |
| Delayed resolve, ring reuse, timestamps-unavailable, wrap, device loss | mock resolve | no robustness cases, no unavailable/device-loss count | (f) robustness + counts |
| Compare total timing to an independent capture | — | no comparison oracle | (j) comparison oracle |
| Do not subtract unrelated CPU/GPU timestamps; no false failure-location precision | — | design constraint | constraint, (d)/(j) |

## Proposed sub-units (order; env-gating marked)

- **(a) census** — this doc.
- **(b) correlation wire format** — carry queue id, device id, timestamp-domain tag and a clock-uncertainty field for
  GPU samples. `Sample` is 32 B with every byte spoken for, so per-sample identity fields **force a CPROF format change**
  (the 6a(d2) spare-`_pad` trick applies to `AllocatorRecord` only, not `Sample`). `CprofHeader` already carries
  `sample_size`, so a bump is *supported*. The leading option (the (d2)-shaped answer) is a **side table** in a NEW
  CPROF section keyed by sample index: old readers ignore the section and load unchanged, new readers join it — no
  per-sample size bump, forward-compatible. Alternative: widen `Sample` (a real bump, every reader). Resolve this before
  writing (b). CPU-side; testable now.
- **(c) separate tracks** — new `Category` values for queue-wait / execution / transfer (Category is a `u8` enum, so
  additional categories need NO format bump); lift the single `g_gpu_thread_index` to per-queue/per-device tracks. This
  subsumes the "transfer/copy-queue" correlation that the row calls "I/O". CPU-side.
- **(d) calibration + explicit uncertainty** — a measured CPU↔GPU offset with a stated error bound carried per GPU
  sample; a CPU-side **fake backend** injecting known GPU ticks as the oracle; teeth = a test that FAILS if the
  uncertainty bound is vacuous (`0`) or the offset is fabricated. CPU-side.
- **(e) frame-graph → crd-perf bridge + resolve wiring** — feed frame-graph pass timings (with pass identity) into the
  GPU track and drive `resolve_gpu_frames` from `frame_mark` (fix the disconnected assembly). **Producer not yet
  located**: `frame_graph.hpp` exposes no per-pass timestamp API, and the only GPU-time accessor found is
  `IGpuCompute::last_gpu_ms` ([compute.hpp:153](../../engine/gpu/gpu-context/include/crd/gpu/compute.hpp), virtual,
  default `0.0` — a compute-path stub, not render-pass timings). Locating the real render-pass timing producer (likely a
  backend impl `crd-gpu-context-vulkan` or a frame execution-result struct, not the public header) is (e)'s FIRST step;
  the DIAG.0 census's "real pass timings used by sandbox" is the pointer to chase, not yet a verified call site. Partly
  env-gated (real timings need a device); the wiring + a fake producer are CPU-side.
- **(f) resolve robustness + counts** — delayed query resolve, query-ring reuse, timestamps-unavailable count (the
  `frame_history_unavailable` analogue), wrap, device loss → fake-backend oracles; a real-device pass is **[env-gated]**.
- **(g) correct the retired-rhi example** — fix `gpu_scope.hpp`'s example (names `crd-rhi-vulkan`) and any public
  example pointing at the retired module. Doc/comment-only; cheap.
- **(h) — REMOVED.** An earlier draft proposed a filesystem `io_scope`; that misread the row (see the I/O note above).
  Transfer/copy-queue correlation is folded into **(c)** as a track. No separate I/O sub-unit.
- **(i) real DX12 + Vulkan backends** — **[env-gated: no GPU lane on this machine]**. Implement the backend artifact +
  a CPU-side oracle; note CI-gating; never skip-as-pass.
- **(j) independent-capture comparison oracle** — compare crd-perf GPU total against an independent measure without
  cross-domain subtraction.
- **(k) acceptance walk + flip** — clause-by-clause against row 078, then Open → Needs CI.

## Constraints the row implies (carried into every sub-unit)

- **"Real" bridge + "explicit" uncertainty**: the acceptance rejects a fabricated offset or a `0` uncertainty; plan a
  measured calibration with a stated bound and a teeth test that fails on a vacuous bound (6a discipline).
- **Env-gating**: GPU/DX12/Vulkan proofs are gated on a device this box lacks — implement artifact + CPU-side fake-backend
  oracle, note CI-gating, do not let a missing GPU become skip-as-pass.
- **`crd::` containers only** — the fake backend and any test harness obey it too.
- **No submit-and-wait stalls** introduced for profiling convenience; resolve stays async.

## Dependencies / cross-module reach (stated honestly)

Much of 6b reaches outside crd-perf: the real pass timings live in **gpu-context** (`frame_graph`), and a shipping
backend belongs in the **rhi** layer (the retired `crd-rhi-vulkan` must be replaced by the current module). The
CPU-side, implementable-now core is (b) wire format, (c) tracks, (d) calibration+uncertainty with a fake-backend oracle,
(e)'s resolve wiring, (g) the example fix, and (f)'s unavailable/device-loss counts. The on-device linked-identity
acceptance and real DX12/Vulkan backends (i) are env-gated and land as artifacts + CPU oracles with CI-gating noted.

Row 078 stays **Open**. Next tick (after the protocol's advisor + CI check, with this census in hand): **(g) first** —
the retired-`crd-rhi-vulkan` example fix is real, cheap, and proves the loop is moving — then **(b)** with the
side-table-vs-Sample-bump decision resolved before any code.

## (g) landed — retired-`crd-rhi` profiler examples corrected

The GPU-diagnostics surface claimed a shipping backend in a module that no longer exists. Corrected to the true state
(comment/doc only — no interface or code-semantics change):

- **[gpu_scope.hpp](../../engine/foundation/perf/include/crd/perf/gpu_scope.hpp)** (header block + the interface,
  `emit_gpu_sample` and `CRD_PERF_GPU_SCOPE` comments): the old `crd::rhi::create_vulkan_profiler_backend` /
  `crd::rhi::CommandBuffer` usage example is replaced by the real contract — construct an `IProfilerGpuBackend`
  implementation (the test `MockGpuBackend` is the reference) and `set_gpu_backend(&be)`, with an opaque `void*` command
  buffer. A production Vulkan/DX12 backend belongs in `crd-gpu-context-vulkan` / `crd-gpu-context-dx12` and is 6b(i).
- **[gpu_scope.cpp](../../engine/foundation/perf/src/gpu_scope.cpp)** (2 sites) and
  **[profiler.cpp](../../engine/foundation/perf/src/profiler.cpp)** `write_external_sample` comment: "supplied by
  `crd-rhi-vulkan` (VulkanProfilerBackend)" → "a gpu-context backend (6b(i)); today only the test mock".
- **[time/gpu_timestamp.hpp](../../engine/foundation/time/include/crd/time/gpu_timestamp.hpp)**: "implementation lives
  in `crd-rhi-vulkan`" → belongs in `crd-gpu-context-vulkan`/`-dx12` (6b(i)); the old RHI was retired (ADR-0105).
- **`resolve_gpu_frames()`**: the false "normally driven from `frame_mark()`" is corrected to "must be called by the
  application once per CPU frame; NOT yet driven from `frame_mark()` (6b(e) wires that)".

**Confirmed facts.** `engine/` has no `rhi` directory — `crd-rhi`/`crd-rhi-vulkan` are fully retired; the current
per-backend modules are `crd-gpu-context-{vulkan,dx12,cuda}`. The other `crd-rhi` mentions across `engine/gpu/*` and
`engine/gpu/kir/*` are ACCURATE retirement history (ADR-0105 "absorbed from crd-rhi", "no dependency on crd-rhi") and
were left untouched. **DG11 in one line:** no production code calls `set_gpu_backend` or `CRD_PERF_GPU_SCOPE` (grepped
`engine/`/`sandbox/`/`tools/`) — the GPU track is inert outside tests until a backend and the frame-graph bridge (6b(e),
(i)) land.

**Scope & teeth.** Comment/doc only; the interface (`Sample`/records/queue/device fields) is untouched — that is (b)/(c).
No teeth are possible for a comment correction; the only check is that the headers still compile, which they do:
`crd-perf` win-debug rebuilds clean (`gpu_scope.cpp` + `profiler.cpp` recompiled, lib linked).

## (b) decision — CPROF side table, signalled by a `flags` bit (no version bump)

The census left open: extend `Sample` vs a side table. The deciding facts, read from
[capture.hpp](../../engine/foundation/perf/include/crd/perf/capture.hpp) `CprofHeader` and
[capture.cpp](../../engine/foundation/perf/src/capture.cpp) `validate_capture_buffer`:

- `CprofHeader` has a **`flags` u64 ("reserved, always 0 in v1")** and a strict `version` (validation rejects a
  mismatched version) and strict `sample_struct_size` / `frame_record_size` equality checks.
- **Validation tolerates a trailing tail**: it rejects a buffer SMALLER than the known sections (`buf.size() <
  min_bytes`) but does NOT reject a LARGER one — trailing bytes after the frame records are ignored.

**Decision: a side table in a new CPROF section appended after the frame records, its presence signalled by a new
`flags` bit — and NOT a `version` bump.** Widening `Sample` would change `sample_struct_size` (strict equality) → old
readers hard-reject: a real break. A `version` bump would also break BOTH directions (old readers reject new files AND,
worse, a new reader would reject every existing capture, violating "compatibility with existing captures"). A `flags`
bit preserves both directions:

- old reader + new file: `version`/sizes unchanged, tail tolerated, unknown flag ignored → reads the base capture;
- new reader + old file: flag bit clear → no section → reads fine;
- new reader + new file: flag bit set → parse the appended section.

This is the 6a(d2)-shaped answer: new semantics through the format's existing extensibility, no break.

**Layout sketch (refined when (b) is coded).** After the frame records, when `flags & kCprofFlagCorrelation`:
`[u32 correlation_count][u32 record_size][CorrelationRecord × count]`. Each record keys on **`(thread_index,
sample_ordinal)`** — samples are per-thread (each `ThreadHeader` owns its sample array via `sample_byte_offset`), so a
global index is meaningless; the pair addresses any sample on any track. Keying on the pair (not just the gpu track's
array) is deliberate: the acceptance requires "linked CPU/task/pass/resource identities", so a CPU submit scope must be
able to carry a pass/resource link too, not only GPU samples. Fields (draft): `u16 thread_index; u16 flags; u32
sample_ordinal; u32 queue_id; u32 device_id; u8 clock_domain; u64 clock_uncertainty_ns; NameId pass_id; NameId
resource_id`.

**Section discovery.** `CprofHeader` also has a live, validation-unchecked `_pad_a` u32 (0 in v1) — use it as the
correlation section's byte offset so the new reader reads the offset directly instead of recomputing `min_bytes` and
trusting the arithmetic. An old writer leaves `_pad_a = 0` and the flag clear (consistent); a new reader treats
flag-clear as "no section". So: flag bit signals presence, `_pad_a` locates it.

**The `(thread, ordinal)` key is SAVE-TIME, and that is the hard part of (b).** `sample_ordinal` is the position in the
thread's COPIED array in the file — but the ring is SPSC with `tail`, `clear_samples` and drop-on-full, so the ordinal a
producer knows at emit (its `head` slot) is NOT the saved position. The correlation section must therefore be built by
`save_capture_to_buffer` WHILE it copies each thread's samples, joining a live producer-side record to each copied
sample by a key the producer actually owns. Plan for (b): **(i)** live side-records keyed by `begin_ns` + `name_id`
(unique per GPU span since `begin_ns` is the resolved GPU begin; a named CPU-collision caveat for tight identically-named
scopes at the same ns — acceptable for the correlation use), joined at save into `(thread, ordinal)`. The alternative —
a producer-stamped correlation id in `Sample` — has no spare byte (it would consume `color_rgba`'s inherit sentinel or
widen `fiber_id`, both real format bumps), so (i) is preferred. The live side-records live **per-thread** (a ring beside
`ThreadRing`) so the c3 single-consumer invariant holds — the copier owns both the sample array and its side-records; a
global MPSC table would need another lock. `clear_samples` must drop the matching side-records too.

Row 078 stays **Open**. Next sub-unit: (b), coding the section above — flag bit + `_pad_a` offset; per-thread live
side-records keyed `begin_ns`+`name_id`, joined at save to `(thread, ordinal)`; reader; a round-trip test plus an
old-capture-still-loads oracle — after the protocol's advisor + CI check.

## (b) landed — CPROF correlation side table (slot-parallel, appended, validated)

Per-sample GPU/queue correlation now round-trips through CPROF without touching the pinned 32 B `Sample` or breaking any
existing capture. The design changed from the census's `begin_ns`+`name_id` join to **slot-parallel** records (advisor
call): the join key was unnecessary.

**Live side (slot-parallel, gpu-track producer only).** `ThreadRing` gains an atomic, lazily-allocated
`CorrelationRecord* correlation` array indexed by the SAME ring slot (`h & mask`) as `samples`.
`enable_thread_correlation(thread_index)` allocates it (cold, idempotent, CAS-installed); `gpu_scope` enables it at
gpu-thread registration. `write_external_sample` (the only gpu-track writer -- `pop_region` is untouched) gained an
optional `const CorrelationRecord*`: when the thread opted in it ALWAYS writes the slot before publishing `head` -- the
given record, or a cleared one -- so a reused slot never shows a stale record ("all-zero = none", `kCorrelationValid`
bit0). The pointer is `std::atomic` because `enable_thread_correlation` may publish the array after `active` is already
set, concurrently with the lock-free reader/writer.

**Copy in lockstep.** `copy_thread_samples_with_correlation` copies a sample AND its correlation record at the same slot
under the SINGLE `reader_busy` hold, so `out[i]` and `corr_out[i]` describe the same sample by construction -- wrap-safe,
no key, and `clear_samples` moving `tail` drops both together. The existing `copy_thread_samples` / `ThreadSamplesView`
path is unchanged (the live UI stays on it).

**Wire format (no break).** The records are written as a SPARSE list (only `kValid` ones, sorted by
`(thread_index, sample_ordinal)`) in a new 8-aligned section appended after the sample arrays, signalled by
`kCprofFlagCorrelation` in `CprofHeader.flags` and located by `CprofHeader.correlation_section_offset` (the former
`_pad_a`, 0 = none). Section = `u32 count; u32 record_size; CorrelationRecord[count]`. The version stays 1, so **old
readers accept new files** (validation tolerates the trailing section) and **new readers accept old files** (flag clear
-> no section). When no thread has correlation the writer reserves nothing and emits byte-for-byte the pre-6b output.
`save_capture_to_buffer` reserves an upper bound (one record per sample on correlation-enabled threads + header +
alignment), accumulates valid records while copying, writes the section, patches flag+offset, and truncates to the
actual end.

**Validation + reader.** `validate_capture_buffer`, when the flag is set, STRICTLY checks the section: 8-aligned, past
the base sections, 8 B header in-bounds, `record_size == sizeof(CorrelationRecord)` (its own version guard), and the
record array within the buffer -- so a corrupt file is rejected, not OOB-read. `CaptureView` gained
`correlation_count()`, `correlation_at(i)`, and `correlation_for(thread, ordinal)` (binary search on the composite key).
Defense-in-depth: the `CaptureView` ctor re-derives the section bounds too, so even a validator bypass cannot OOB.

**Scope.** (b) lays the wire and the gpu-track producer only. CPU-scope -> pass/resource linkage (the acceptance's
"CPU/task/pass" identities) needs a thread-local correlation context set by the frame-graph executor and would put a
branch in `pop_region`'s hot path -- that is DIAG.6b(c)/(e). The section format already supports any `thread_index`, so
those sub-units add producers without touching the format. The zero-overhead gate is unchanged (`[gate]` 5/2 --
`pop_region` never touched).

**Verified.** New [`test_diag_correlation.cpp`](../../tests/foundation/perf/test_diag_correlation.cpp)
(`[perf][diag][correlation]`, 5 cases): round-trip (records resolve by `(thread, ordinal)`, `queue_id`/`pass_id`
recovered); slot-parallel across a ring wrap (small ring + `clear_samples` pushes `tail` off zero so the live window
crosses the mask wrap; each record's `queue_id` must still equal its sample's `begin_ns`); mixed valid/none plus a
slot-REUSE part (fill valid -> clear -> refill with none -> no stale record); existing-capture compat (no correlation ->
flag clear/offset 0, base sections load unchanged); corrupt section rejected by validation. **Teeth** (all observed,
then restored): writing correlation to `(h+1)&mask` broke the wrap test with an exact off-by-one (`3006 == 3007`);
skipping the clear-on-nullptr made stale batch-1 records reappear for batch-2 samples (`correlation_count() 8 == 0`);
disabling the validator's bounds check let the corrupt buffer pass. Full `crd-perf-tests` **win-debug 1049/175** (was
918/170 + the 5 correlation cases); **win-asan** `[correlation],[capture],[sample-ring]` **219/19** clean; zero-overhead
gate `[gate]` 5/2 unchanged; win-shipping `/W4 /WX` `crd-perf` clean; `crd-perf-ui` builds; `check_no_std_containers`
PASS; both validators PASS; no repo-root scratch.

Post-landing advisor pass added two tightenings (both landed): (1) the `capture_view.hpp` `correlation_for` doc-comment
now states that `thread_index` is the capture's `ThreadHeader.thread_index`, which the writer keeps equal to the dense
thread position -- so it matches the index `thread_samples`/`thread_name` take (the multi-thread consumer no longer has
to infer that invariant); (2) `correlation_at()` gained direct coverage in the round-trip case (index 0 is thread 0 /
ordinal 0 and joins back via `queue_id`, out-of-range index is nullptr) -- `[correlation]` now **134 assertions / 5
cases**. The skipped ASan-OOB teeth stays skipped by design: the ctor re-check is genuine defense-in-depth, and teeth-3
(validator flip) already shows the load-bearing check; manufacturing an OOB by disabling both layers would prove nothing
about the real path, which win-asan exercises clean.

Row 078 stays **Open**. Next sub-unit: **(c)** separate tracks -- new `Category` values for queue-wait/execution and a
transfer/copy-queue track, lifting the single `g_gpu_thread_index` to per-queue/per-device tracks. (`Category` already
has `Pass`/`Io`/`Wait`/`Gpu` -- (c) wires producers to them.) After the protocol's advisor + CI check.

## (c) landed -- separate per-(device,queue) GPU tracks (execution / transfer / queue-wait)

**Decision on the census's own tension.** The (a) plan said "new `Category` values"; the enum already carries
`Gpu`/`Io`/`Wait`/`Pass` (`sample.hpp`), so (c) **reuses** them -- no new enum value, no `Sample` byte change, no format
bump. Producers are routed to the existing categories via a `GpuSampleKind` (Execution->`Gpu`, Transfer->`Io`,
QueueWait->`Wait`); an enum, not a raw `Category`, so a backend cannot ship an unrelated category onto a GPU track.

**Latent defect found and fixed (the point of (c)).** The single-track code called `register_thread("gpu")` from the
resolve/emit thread. `register_thread` registers the *calling* thread and caches TLS -- so on an already-registered
thread (e.g. `main`, index 0) it took the idempotent refresh path and **re-owned the caller's own ring's name to
"gpu"**, aliasing the GPU track onto `main`. Every prior `[gpu]` test passed with the two tracks aliased. Lifting to
per-(device,queue) tracks is impossible through a TLS-caching API, so (c) adds a new primitive.

**Implementation.**
- `crd::perf::register_external_track(const char*)` (`profiler.hpp`/`.cpp`): a dedicated ring slot that is NOT the
  caller's own track and touches no thread-local state. Refactored `register_thread`'s fresh block into a shared
  `detail::allocate_ring_slot(state, name, assert_on_full)`; `register_thread` = slot + TLS bind + the original hard
  `CRD_ASSERT_MSG` on a full table; `register_external_track` = slot only, **no assert** -> returns `kInvalidThread`,
  caller degrades. No refresh/re-own path (every call is a fresh slot; the caller owns the identity->index mapping). The
  6a(d) refresh/re-own behavior of `register_thread` is untouched. Disabled-branch stub added.
- `gpu_scope.cpp`: replaced the single `g_gpu_thread_index` with a bounded track table (`kMaxGpuTracks = 16`, leaving 48
  of `kMaxThreads`=64 for CPU/job threads) keyed `(device_id, queue_id)` -> external-track index, each named
  `"gpu d<dev> q<queue>"` and correlation-ready. Fast emit path scans lock-free up to a release-published count; cold
  registration under an `atomic_flag` spinlock with a re-scan (a second emitter can't corrupt or double-register). Keys
  beyond the cap fold onto a shared `"gpu overflow"` track and bump `gpu_track_overflow_count()` (per folded SAMPLE --
  over-cap keys are not tabled, so each such emit re-folds under the lock) -- folded, never dropped silently.
  `reset_gpu_state()` (shutdown) clears the whole table under the lock.
- Public API (`gpu_scope.hpp`): `GpuTrackKey{device_id, queue_id}`, `enum class GpuSampleKind`, `kMaxGpuTracks`,
  `emit_gpu_sample_on(Sample, GpuTrackKey, GpuSampleKind)` (forces category+thread from kind/track, attaches a
  `CorrelationRecord` with the (device,queue) identity + pass name; `clock_domain`/`clock_uncertainty_ns` stay 0 for
  (d)), and `gpu_track_overflow_count()`. `emit_gpu_sample(Sample)` is now the single-track shim -> default execution
  track (0,0); `gpu_thread_index()` returns that default track (documented) -- **behavior change: the GPU track no
  longer aliases the registering CPU thread; `gpu_thread_index()` returns a distinct slot.**

**Tests** (`test_diag_gpu_tracks.cpp`, `[perf][diag][gpu-tracks]`, in the CMake list): (1) the alias-regression oracle
-- after a GPU emit, `gpu_thread_index() != current_thread_index()` and the caller's track is still named `"main"`; (2)
three keys `(0,0)/(0,1)/(1,0)` -> three distinct tracks/names, each sample's category matches its kind and its
`correlation_for(idx, ord)->{device_id,queue_id,pass_id}` matches its key after a CPROF round-trip; (3) idempotent --
the same key twice is one track holding both samples; (4) overflow -- exactly `kMaxGpuTracks` keys then three emits on one
over-cap key fold onto `"gpu overflow"` and bump `gpu_track_overflow_count()` to 3 (per sample, not per key), the folded
sample survives. Two teeth observed then restored:
swapping the primitive back to `register_thread` flips the alias-regression + collapses the tracks (3 failed/1 passed);
collapsing the lookup to entry 0 fails three-keys/idempotent/overflow (3 failed/1 passed).

**Gates.** win-debug full `crd-perf-tests` **1127/179** (was 1049/175; +78 assertions / +4 cases, no regressions);
win-asan `[gpu-tracks],[gpu],[correlation]` **234/17** clean; `[gpu]` 25/8 (existing tests green -- alias fix compatible);
zero-overhead `[gate]` 5/2 unchanged (`pop_region` untouched); win-shipping `/W4 /WX` `crd-perf` + `crd-perf-ui` clean;
`check_no_std_containers` PASS (fixed C array + atomics, no owning STL); both validators PASS (9705 links); no repo-root
scratch. Retired-rhi/single-track file-header comments in `gpu_scope.{hpp,cpp}` updated to the multi-track model.

Row 078 stays **Open**. Producers wired to real backends are DIAG.6b(e)/(i); a QueueWait *producer* (a submit
timestamp) is (d)/(f) -- (c) only plumbs the track/category so a backend can route it. Next sub-unit: **(d)** calibration
+ explicit uncertainty (fake-backend oracle; teeth = a vacuous `0` bound fails).

## (d) landed -- CPU<->GPU clock calibration + explicit uncertainty

**Placement.** The pure math lives in `crd/time/gpu_timestamp.hpp` (crd-time already owns the GPU-clock conversion
types, and crd-perf DEPENDS on it): `GpuClockCalibration { i64 cpu_ns; u64 gpu_ticks; f64 ns_per_tick; u64
max_deviation_ns }`, a `constexpr gpu_round_ns(f64)->i64` (round-half-away, static_assert'd on the +/-0.5 boundaries --
`std::llround` is not constexpr on MSVC and an implicit `f64->i64` trips `/W4 /WX`), `gpu_ticks_to_cpu_ns(calib, ticks)`
(signed tick delta -- a span may precede the instant), and `gpu_span_uncertainty_ns(prev*, cur, begin_ticks)`. The
per-device store + emit wiring + flags live in crd-perf.

**No forbidden subtraction.** A calibration is ONE instant sampled on both clocks (Vulkan
`vkGetCalibratedTimestampsEXT` + `maxDeviation`; DX12 `GetClockCalibration`). The offset is never derived by subtracting
a CPU-submit from a GPU-execute timestamp -- that is the design's banned "subtract unrelated CPU/GPU timestamps", stated
in the header.

**Two-sample drift, not a ppm constant.** With one calibration the bound is `cur.max_deviation_ns`. With a previous one,
`observed_drift = (cur.cpu - prev.cpu) - round((cur.ticks - prev.ticks) * period)` is a MEASURED quantity; the bound
widens by `|observed_drift|` (plus `prev.max_deviation_ns` when the span predates `cur`, i.e. it is bracketed). A
fabricated ppm is exactly the "fabricated" thing the teeth reject.

**The vacuous-zero fix (flags/sentinels in `sample.hpp`).** `kCorrelationCalibrated = 0x2` (bit1; 40 B pin holds; old
readers ignore it -- no format change), `kUnknownClockUncertainty = ~0ULL`, `kClockDomainCpu = 0`,
`kClockDomainGpuBase = 1`. **Rule, stated once:** flag set => `begin_ns/end_ns` on the CPU timeline AND
`clock_uncertainty_ns` is a real bound strictly > 0; flag clear => uncertainty MUST be the sentinel, never a misleading
0. The ns-native `emit_gpu_sample_on` (backend already converted) therefore writes sentinel + flag clear -- crd-perf did
not calibrate, so it does not vouch. No `capture_view` reader assumed 0 meant anything (grepped).

**API + store (gpu_scope).** `kMaxGpuDevices = 4`; `set_gpu_clock_calibration(device_id, calib)` (keeps cur + prev per
device, cold path under `g_track_lock`); tick-native `emit_gpu_span_on(key, kind, name, begin_ticks, end_ticks,
ns_per_tick)` -- crd-perf does the conversion and stamps the record, so a backend cannot hand-roll an offset through it;
`uncalibrated_span_count()`. The calibration snapshot is read under `g_track_lock` AFTER `ensure_gpu_track` has released
it (never a nested acquire; `atomic_flag` is not recursive), and `set_gpu_clock_calibration` writes the (prev, cur) pair
under that same lock -- so the snapshot is never torn (mutual exclusion, not tolerance). Re-init safety is the
profiler's existing hot-path contract: `write_external_sample` is a 6a(e) hot writer (bare relaxed `g_state` load +
`ring.active` check, NOT the `g_in_flight` reader drain), so both this path and the existing `emit_gpu_sample` rely on
producers quiescing before shutdown -- the resolve thread being one such producer. Uncalibrated device => raw GPU-domain
ns (`round(ticks * ns_per_tick)`, no offset), `clock_domain = kClockDomainGpuBase + device`, flag clear, sentinel,
counted -- "retain separate tracks". `reset_gpu_state` drops the calibrations + counter under the lock.

**Not in (d) (recorded for (f)):** `emit_gpu_span_on` does no wrap guard -- a GPU counter with `timestampValidBits < 64`
can hand back `end_ticks < begin_ticks`, and the signed math then yields a reversed span (`end_ns < begin_ns`). Masking
`timestampValidBits` and the wrap/device-loss handling are (f)'s clause by name, not (d)'s.

**Tests** (`test_diag_gpu_calibration.cpp`, `[perf][diag][gpu-calib]`, in the CMake list) against a fake backend with a
known truth `T(ticks)`: (1) a calibrated span -- flag set, `unc != sentinel`, `unc > 0`, `unc >= max_dev`, and
**honesty** `|begin_ns - T(ticks)| <= unc` after a CPROF round-trip; (2) a span between two calibrations under a real
linear drift -- `unc >= |drift|` and honesty holds against the drifted truth; (3) an uncalibrated device -- flag clear,
`unc == sentinel`, `clock_domain == GpuBase + dev`, raw-domain ns, `uncalibrated_span_count() == 1`; (4) the ns-native
path -- flag clear, sentinel, `clock_domain == Cpu`. Four teeth observed then restored: force `unc = 0` (fails test 1's
bound + honesty); drop the offset (fails test 1 + test 2 honesty by `kBase`); sentinel -> 0 on the uncalibrated path
(fails test 3); ignore the inter-calibration drift (fails test 2's `unc >= |drift|` + honesty).

A fifth case pins the calibration **shift** semantic: three calibrations with a big drift A->B then a small one B->C, a
span after C -- the bound must track the RECENT drift (prev == B), so `unc < 100` while a stale-prev bug (prev stuck at
A) would give ~1015. Teeth: shifting prev only on the first update fails it.

**Gates.** win-debug full `crd-perf-tests` **1171/184** (was 1128/179; +43 assertions / +5 cases, no regressions);
win-asan `[gpu-calib],[gpu-tracks],[gpu],[correlation]` **278/22** clean; `[gate]` 5/2 unchanged; win-shipping `/W4 /WX`
`crd-perf` + `crd-perf-ui` + `crd-time` clean (the constexpr `f64->i64` math survives `/WX`); UI tests 66/16;
`check_no_std_containers` PASS (fixed C arrays + atomics); both validators PASS (9705 links); no repo-root scratch.

Row 078 stays **Open**. Next sub-unit: **(e)** frame-graph -> crd-perf bridge + drive `resolve_gpu_frames` from
`frame_mark` -- first step is locating the real render-pass timing producer (the census flags it as not yet a verified
call site).

## (e) landed -- frame_mark drives the GPU frame lifecycle; queue contract; producer verdict

**Producer verdict (Outcome A, but the real hookup is (i)).** The census's "real DX12/Vulkan pass timings used by the
sandbox" is CORRECT: `IFrameGraph::pass_gpu_ms(i)` / `gpu_ms_total()` are really overridden by device timestamp queries
in [dx12_raster_context.cpp:7327](../../engine/gpu/gpu-context-dx12/src/dx12_raster_context.cpp) and
[vulkan_raster_context.cpp:8091](../../engine/gpu/gpu-context-vulkan/src/vulkan_raster_context.cpp), and consumed at
[scene_renderer.cpp:7043](../../engine/rendering/scene-render/src/scene_renderer.cpp) (`stats.gpu_ms =
fg.gpu_ms_total()`). BUT: (1) those modules do NOT depend on crd-perf and crd-perf does not depend on them -- neither
can name the other's types; (2) the producer exposes an aggregate `double` **milliseconds** per pass, not a
(begin_ticks, end_ticks) pair on a monotonic clock, so it cannot feed `emit_gpu_span_on` directly; (3) non-zero values
need a real GPU device. So the real hookup is a device-gated adapter that depends on both sides -- **DIAG.6b(i)**, not a
3-line wire. (e) delivers the driving lifecycle + the bridge contract + a fake producer proving the chain end to end.

**The disconnected assembly, fixed.** No engine code called `begin_frame`/`end_frame`/`resolve_gpu_frames` (grepped),
and `gpu_scope.hpp` claimed `resolve_gpu_frames` was driven by `frame_mark` and `end_frame` was "called from
resolve_gpu_frames" -- two false header claims. Now `frame_mark()` drives the lifecycle in order **`end_frame()` ->
`resolve_completed_frames()` -> `begin_frame(next)`** (via `detail::gpu_frame_advance(frame_index + 1)` at the end of
`frame_mark`, under its `StateGuard` so shutdown's 6a(e) drain waits for it); `set_gpu_backend` opens the first frame on
install so pre-first-mark spans have an open frame. `resolve_gpu_frames()` stays resolve-only + idempotent (a legacy
manual call after a `frame_mark` is a harmless no-op; it never ends/begins a frame). Both header lies corrected.

**Cost.** `frame_mark` gains one acquire load + a not-taken branch when no backend is installed. `[gate]` covers
push/pop, not `frame_mark`, so this is stated here rather than gate-enforced.

**Queue contract (gpu_scope.hpp).** `kGpuQueueGraphics = 0`, `kGpuQueueAsyncCompute = 1`, `kGpuQueueTransfer = 2` (plain
`u32`; crd-perf never names `FgQueue`). A producer maps its own queue enum onto these so tracks mean the same thing
across backends and the UI can label them -- this is the contract (i) implements. (c)'s `gpu d<dev> q<queue>` track names
are unchanged.

**Fake producer + tests** (`test_diag_gpu_bridge.cpp`, `[perf][diag][gpu-bridge]`, CMake list): `FakeFrameGraphBackend`
(fixed C arrays, 2 frames in flight) records per-frame spans and, on resolve, emits the out-of-flight frame via the
tick-native `emit_gpu_span_on` -- so a stream of `frame_mark()`s alone drives (b)->(c)->(d). Oracles: (1) lifecycle --
install => `begin_frame_calls == 1` at `frame_count()`, three marks (no manual resolve) => end/resolve/begin ==
3/3/4, indices consecutive; (2) routing -- a graphics-queue execution span and a transfer-queue copy recorded in frame 0
appear on NO track after one mark (in flight) and on `gpu d0 q0` (Gpu) / `gpu d0 q2` (Io) after the second, each
`correlation_for(...)->queue_id` == the contract constant, flag calibrated; the transfer track is ABSENT after one mark
(lazy registration) and present after two. (3) `resolve_gpu_frames` resolve-only -- after a mark + a manual resolve,
`end==1`, `begin==2`, `resolve==2`. Two teeth observed then restored: unwire `gpu_frame_advance` (all three cases fail);
skip `resolve_completed_frames` (the sed caught both call sites -- `gpu_frame_advance` and `resolve_gpu_frames` -- and all
three cases fail). The existing `begin_frame/end_frame forwarded` `[gpu]` test was made delta-based (install now opens a
frame) -- expectation updated, never weakened.

**Not in (e) (recorded for (f)):** `set_gpu_backend(nullptr)` abandons the outgoing backend's open frame (no `end_frame`
on clear) and a reinstall opens a fresh one -- harmless for the mock/fake, but a real query-pool backend must reclaim in
its own destructor; (f)'s device-loss clause decides this. (The wrap guard is likewise (f)'s -- see (d)'s note.)

**Gates.** win-debug full `crd-perf-tests` **1201/187** (was 1171/184; +30 assertions / +3 cases, no regressions);
win-asan `[gpu-bridge],[gpu-calib],[gpu-tracks],[gpu]` **170/20** clean and `[quiescence]` **12/4** clean (frame_mark is
the 64-iter race path and it changed); `[gate]` 5/2 unchanged; win-shipping `/W4 /WX` `crd-perf` + `crd-perf-ui` clean;
UI tests 66/16; `check_no_std_containers` PASS (the fake uses fixed C arrays); both validators PASS (9705 links); no
repo-root scratch.

Row 078 stays **Open**. Next sub-unit: **(f)** resolve robustness + counts (delayed resolve, query-ring reuse,
timestamps-unavailable count, wrap [`emit_gpu_span_on` has no wrap guard -- (f) owns `timestampValidBits` masking],
device loss) against fake-backend oracles; a real-device pass is env-gated.

## (f) landed -- resolve robustness + counts (wrap, invalid, unavailable, reuse, delayed, device loss)

**Two retractions from the (e) wakeup note, corrected here:** the wrap width is per-DEVICE state, not an
`IProfilerGpuBackend` vtable getter (emit_gpu_span_on is white-box, called with no backend in every calibration test);
and "delayed resolve lands in ordinal order" is wrong -- ring ordinals are emission order and the UI sorts by `begin_ns`,
so the real invariant is **lands exactly once** (never zero, never twice).

**Narrow-counter wrap (crd-time).** `constexpr i64 gpu_tick_delta(ticks, ref, valid_bits)` masks both to the window and
sign-extends (`valid_bits >= 64` branches away the `1ULL << 64` UB and returns the plain cast). `static_assert`s pin
B=32 forward/backward-across-wrap and B=64 unchanged. `gpu_ticks_to_cpu_ns` and `gpu_span_uncertainty_ns` gained a
trailing `valid_bits = 64U` and route every tick delta (including the drift's `cur.ticks - prev.ticks`) through it. The
helpers are byte-identical for B=64; `emit_gpu_span_on`'s end-of-span is now `begin_ns + round(duration * period)`
(derived from the repaired duration, never a re-masked end) -- a <=1 ns rounding difference from the old
`gpu_ticks_to_cpu_ns(end)` for a non-integer period, inside any real uncertainty; the exact-period (d)/(e) tests are
unchanged.

**Wrap rule (emit_gpu_span_on).** Per-device `valid_bits` (set by `set_gpu_timestamp_valid_bits`, default 64, clamped
[1,64]) is snapshotted under `g_track_lock` beside the calibration. The duration is `gpu_tick_delta(e_masked, b_masked,
valid_bits)`: `< 0` -> not a repairable wrap (a 64-bit counter cannot wrap in a frame) -> **drop + `gpu_invalid_span_count`**,
never a reversed span; masked `e < b` with a forward modular delta -> single wrap **repaired + `gpu_wrap_repaired_count`**.
End is always `begin_ns + round(duration * period)` (derived from the repaired duration, never a re-masked end).

**Backend-reported counters** (crd-perf cannot see inside a query pool, so the producer calls these):
`note_gpu_timestamps_unavailable(n)` -> `gpu_timestamps_unavailable_count()` (query results unreadable -- counted, never
fabricated); `note_gpu_query_slot_reused(n)` -> `gpu_query_slot_reused_count()`; `note_gpu_device_lost(device, dropped)`
-> `gpu_device_lost_count()` + `gpu_pending_frames_dropped_count()`. All six reset by `reset_gpu_state`; disabled stubs
added.

**set_gpu_backend(nullptr) decision (was owed since (e)):** it does NOT call `end_frame` on the outgoing backend -- a
lost device may reject the call, and the backend's destructor reclaims its query pool. The abandoned open frame is
harmless (no further spans arrive). Stated in the code comment.

**Tests** (`test_diag_gpu_resolve.cpp`, `[perf][diag][gpu-resolve]`, CMake list) with a `FakeRobustBackend` (fixed C
arrays, knobs `record`/`set_unavailable`/`hold_frame`/`lose_device`): (1) 32-bit wrap uncalibrated -> forward span,
duration `0x20 * period`, `gpu_wrap_repaired_count()==1`; (2) 32-bit wrap under calibration -> correct forward duration
on the CPU timeline, flag calibrated; (3) 64-bit `end<begin` -> dropped, `gpu_invalid_span_count()==1`, no sample; (4)
unavailable/reuse/device-loss via `frame_mark` -> the right counter increments and NO fabricated samples (reuse emits
`kSpans`, dropped extras counted; device loss drops both pending frames, counts once, a further mark is a silent no-op);
(5) a frame held 3 extra marks lands **exactly once** (2 samples, not zero, not doubled). Teeth: force full-width delta
(the two wrap cases plus one downstream counter check flip); zero the four backend-noted increments (their counter CHECKs
fail) -- observed then restored. (The counter-batch restore required a targeted Read+Edit fix after a sed collision on
two identical lines; final state verified green. Lesson for (i)/(j): use unique per-site break markers.)

**Gates.** win-debug full `crd-perf-tests` **1234/192** (was 1201/187; +33 assertions / +5 cases, no regressions);
win-asan `[gpu-resolve],[gpu-bridge],[gpu-calib],[gpu-tracks],[gpu],[quiescence]` **219/29** clean; `[gate]` 5/2;
win-shipping `/W4 /WX` `crd-perf` + `crd-perf-ui` + `crd-time` clean (every shift + `f64->i64` explicitly cast); UI
66/16; `check_no_std_containers` PASS (fake uses fixed C arrays); both validators PASS (9705 links); no repo-root
scratch.

Row 078 stays **Open**. Remaining before the (k) acceptance walk: **(i2)** the raster-context overrides that retain the
raw tick pairs (landed; real-device verified — see the (i2) section). **(j)** landed — see the section after (i).

## (i) landed -- the frame-graph -> crd-perf GPU bridge (interface + adapter module + fake proof)

**The fork, reconciled once with the advisor after the reads.** The (f) wakeup note ("convert ms->ns and emit via
`emit_gpu_sample_on`") was **retracted**: `pass_gpu_ms(i)` is a DURATION with no begin, so emitting a positioned sample
from ms alone fabricates a begin -- exactly what (d)/(f) forbid. A span that cannot be PLACED must be COUNTED
unavailable, never invented. That splits (i) two ways: **(i-ticks)** extend `IFrameGraph` to expose the raw per-pass
ticks, then a bridge places them; **(i-ms-only)** the bridge emits no positioned samples and counts every pass
unavailable forever. Read 1 found both raster backends convert ticks->ms from STACK LOCALS in `resolve_timestamps()`
and retain only `m_pass_ms[i]`, so tick retention is invasive (a member add + loop change in two ~8k-line, GPU-lane-only
files). Read 4 found NO module depends on both perf and gpu-context (ceir-host->perf, ceir-gpu->gpu-context; only
test-deps link both). Verdict: **(i-ticks)**, scope-valved -- the interface extension + a fake-verified adapter module
land now (fully provable on this box); the two blind backend overrides are **(i2)**. (i-ms-only) was rejected: it ships
an artifact with no positive proof path that (i2) would immediately rewrite.

**Interface (`frame_graph.hpp`, appended at the END of the `IFrameGraph` vtable, D135).** Five virtuals with
"unavailable" defaults, so every existing backend compiles unchanged and honestly reports "no ticks":
`pass_gpu_ticks(i, u64& begin, u64& end) -> bool` (false = not retained), `gpu_timestamp_period_ns() -> double`
(0 = unknown), `gpu_timestamp_valid_bits() -> u32` (64 = full width, narrower lets the bridge repair a wrap),
`pass_kind(i) -> FgPassKind`, `pass_queue(i) -> FgQueue`. The device-level availability gate REUSES the existing
`gpu_timing_available()` (REN-8) -- no parallel flag. **No backend overrides these yet** -- stated in the header and
here; that is (i2).

**Module (`engine/gpu/perf-gpu-bridge`, `crd-perf-gpu-bridge`; `DEPENDS core gpu-context perf time`,
`TESTS gpu/perf-gpu-bridge`).** Its own module because it is the ONE place that names both perf and gpu-context, whose
mutual independence must stay intact; a leaf (nothing depends back on it). Registered in the root `crd_module()` list and
`tests/CMakeLists.txt`; added to the `docs/systems/README.md` module route (check-master-plan requires every registered
module have one).

**Adapter (`FrameGraphGpuBackend : IProfilerGpuBackend`).** The frame graph times itself, so `begin_span`/`end_span` are
inert no-ops (returning `kInvalidGpuSpan`) and `begin_frame`/`end_frame` are bookkeeping. All the work is in
`resolve_completed_frames()` (driven once per CPU frame by `frame_mark`): if `!gpu_timing_available()` return (device
has no timestamps -- nothing to place, nothing to count, a different signal from a per-pass read failure); else set the
device valid-bits (every resolve, idempotent -- survives a shutdown->init that reset the device to 64 bits); then for each pass, `pass_gpu_ticks` true -> `emit_gpu_span_on({device, map_queue(pass_queue)},
map_kind(pass_kind), intern_name(pass_name), begin, end, period)`; false -> `note_gpu_timestamps_unavailable(1)`.
`map_kind`: Transfer -> `GpuSampleKind::Transfer` (Category::Io), every other kind -> `Execution` (Category::Gpu).
`map_queue`: `FgQueue::Async` -> `kGpuQueueAsyncCompute`, else `kGpuQueueGraphics`. Pass names are interned per pass per *changed* frame (`intern_name` self-dedups; cold path -- no per-pass NameId cache). No calibration is installed (no
CPU+GPU paired-sample source exists yet), so spans land uncalibrated in the raw GPU-tick domain (sentinel uncertainty,
counted by `uncalibrated_span_count()`) -- (d)'s "retain separate tracks" path.

**Dedupe invariant.** The frame graph exposes no execute counter, so a second `frame_mark()` with no intervening
`execute()` would re-present identical timings. The bridge dedupes on an FNV-1a content fingerprint over
(pass_count, per-pass avail/begin/end/kind/queue); an app calling `execute()` at most once per `frame_mark()` therefore
emits each frame's spans exactly once.

**Tests** (`tests/gpu/perf-gpu-bridge/test_frame_graph_gpu_backend.cpp`, `[perf][diag][gpu-bridge-real]`) with a
`FakeFrameGraph : IFrameGraph` (fixed C arrays; the timing getters driven from a table, every other pure virtual an
inert stub): (1) **default-unavailable -- the verdict a real device gives today**: timing available but
`pass_gpu_ticks` never overridden -> zero samples, `gpu_timestamps_unavailable_count()==pass_count`; (2) all passes
carry ticks -> one span per pass on the mapped track (`gpu d0 q0` x2, `gpu d0 q1` x1), right categories/durations,
uncalibrated (flag clear, sentinel uncertainty, right `queue_id`/`pass_id`); (3) mixed -> available placed, unavailable
counted; (4) dedupe -> three marks, one execute, emitted exactly once; (5) device reports no timing -> the bridge does
nothing and counts nothing. Teeth (unique markers, observed-then-restored): **TEETH-A** map every queue to Graphics ->
`gpu d0 q1` count flips; **TEETH-B** fabricate a begin=0 span for an unavailable pass -> the default-unavailable
"zero samples" + unavailable-count checks flip; plus a dedupe break -> every counter doubles.

**Gates.** win-debug bridge test **34/5**; win-debug full `crd-perf-tests` **1234/192** (unchanged -- no perf source
touched); win-asan bridge test 34/5 clean; win-shipping `/W4 /WX` `crd-perf-gpu-bridge` + `crd-gpu-context` +
`crd-gpu-context-dx12` + `crd-gpu-context-vulkan` + `crd-perf` + `crd-time` clean (the additive header change compiled by
every consumer, zero warnings); UI 66/16; `check_no_std_containers` PASS (covers the new dirs); both validators PASS
(9710 links, 98 modules); no repo-root scratch.

**(i2) -- the named follow-up.** In both `resolve_timestamps()` (`dx12_raster_context.cpp` ~L8246,
`vulkan_raster_context.cpp` ~L9515) retain the raw `a`/`b` tick pairs into a `crd::u64 m_pass_ticks[kMaxTimedPasses*2]`
member (they are stack locals today), and override the four new getters (`pass_gpu_ticks`, `gpu_timestamp_period_ns`,
`gpu_timestamp_valid_bits`, `pass_kind`/`pass_queue`) accordingly. Blind (no GPU lane on this box) and env-gated;
compile-checked in win-shipping. Until it lands the bridge honestly emits zero positioned samples on real hardware --
proven, not a silent shortfall.

Row 078 stays **Open** (flips only at (k)).

## (j) landed -- independent-capture comparison oracle (placed total vs the frame graph's own reduction)

**"Independent capture" on this box = the frame graph's own `gpu_ms_total()`.** The acceptance
([runtime-diagnostics.md#diag-6b](../design/runtime-diagnostics.md#diag-6b)) says "Compare total timing to an
independent capture" and names NO external tool (grepped: no PIX/RenderDoc/Nsight). The independent measure provable
here is REN-8's backend-side reduction `gpu_ms_total()` ("first pass start -> last end", in ms) -- produced by a
DIFFERENT code path from crd-perf's tick->ns conversion in the bridge, so comparing the two catches every bridge bug
class (wrong period, wrong valid-bits, dropped pass, wrong reduction, dedupe error). External-tool comparison (a real
PIX/GPA capture) is env-gated -- no GPU lane on this box -- and is a superset of this check, not a substitute for it.

**Where.** Appended to `FrameGraphGpuBackend::resolve_completed_frames()`, after the emit loop, inside the
once-per-changed-frame body (so each frame is counted exactly once; the dedupe gate already suppresses re-resolves).

**Reduction = SPAN, not sum.** The placed total is `gpu_round_ns(gpu_tick_delta(max_end, min_begin, valid_bits) *
period)` over the placed passes -- first begin to last end, gaps INCLUDED, matching `gpu_ms_total`'s definition. Every
endpoint is a delta from the first placed pass's begin via the same crd-time helper the emit path uses, so a
narrow-counter wrap is handled identically. Summing per-pass durations (gaps excluded) is the wrong reduction; a tooth
proves it.

**Three counted verdicts, never skip-as-pass** (`total_match_count` / `total_mismatch_count` /
`total_incomparable_count` + `last_total_discrepancy_ns`): **incomparable** = the placed total is incomplete -- a pass
unavailable or `period == 0` -- so it is NOT compared. That is the honest verdict a real device gives today (all passes
unavailable, (i2) pending), asserted by a test, not silently skipped. An **empty frame** (no passes) counts nothing
(early return) -- it is not "incomparable," so a headless app never accrues verdicts.

**Per-frame, not once-per-process (dedupe fold).** On the (i2)-pending path every pass is unavailable, so the per-pass
fingerprint bits are identical frame to frame -- a genuine new execute would dedupe and the incomparable verdict would
be counted once per PROCESS. So `gpu_ms_total()` is folded into the dedupe fingerprint (`std::bit_cast` to bits): a real
execute changes the total, making each a distinct frame counted separately. A tooth (neutralise the fold) proves it: the
per-frame test drops from 2 to 1.

**Derived tolerance, not a magic constant.** Both sides are GPU-domain durations of the SAME tick span, differing only
by rounding: each applies one `gpu_round_ns` (+-0.5 ns) and the reference's ms<->ns round-trip adds sub-ns. A
disagreement as large as one whole tick (`period` ns) would mean the two sides counted a different span -- a real
mismatch. Bound: `tolerance_ns = gpu_round_ns(period) + 1`. Tested at the boundary (period == 2.0 -> tolerance 3 ns:
off by 3 -> match, off by 4 -> mismatch).

**Constraint clauses, in code.** The comparison is duration-vs-duration in ONE domain -- never `cpu_ns - gpu_ns`; a
mismatch reports only the discrepancy MAGNITUDE, never which pass ("do not infer exact GPU failure location").

**Tests** (`tests/gpu/perf-gpu-bridge/test_frame_graph_gpu_backend.cpp`, `[perf][diag][gpu-total]`; `FakeFrameGraph`
gains an overridable `gpu_ms_total()`): (a) exact match -> match 1, discrepancy 0; (b) reference off by 1000 ns ->
mismatch 1, discrepancy -1000 (sign+magnitude, no per-pass blame); (c) boundary at tolerance -> match, one ns past ->
mismatch; (d) one pass unavailable -> incomparable 1, never compared (the box's real verdict); (e) a gap between passes,
reference = span -> match (proves the span reduction); (f) two all-unavailable frames differing only in `gpu_ms_total`
-> incomparable **2** (per-frame, via the fold); (g) an empty frame -> all three counters 0. Teeth (warning-clean
one-line flips under /W4 /WX via a `pick_total(span, sum)` helper that names both operands): **TEETH-C** return the
sum-of-durations reduction -> every span-based check flips, (e) is the discriminating one; **TEETH-D** convert with
`period * 2` -> (a) flips; and neutralising the fingerprint fold -> (f) drops 2->1. Observed then restored.

**Gates.** win-debug bridge **57/12** (was 34/5; +23 assertions / +7 cases); full `crd-perf-tests` **1234/192**
(unchanged -- no perf source touched); win-asan bridge 57/12 clean; win-shipping `/W4 /WX` and `win-clang-cl-shipping`
`crd-perf-gpu-bridge` clean; container check PASS; both validators PASS.

Row 078 stays **Open**. Remaining: **(i2)** landed (below), then **(k)** the acceptance walk that flips 078 Open ->
Needs CI.

## (i2) landed -- DX12 + Vulkan retain raw per-pass ticks and override the (i) getters (real-device verified)

**Both raster backends now override the four (i) getters** (`pass_gpu_ticks`, `gpu_timestamp_period_ns`,
`gpu_timestamp_valid_bits`, `pass_kind`/`pass_queue`) instead of the "unavailable" defaults. Each `resolve_timestamps()`
retained only `m_pass_ms[i]` from stack locals; it now also stores the raw begin/end pair in a
`crd::u64 m_pass_ticks[kMaxTimedPasses*2]` member (gated by `m_timed_passes`, so it goes stale in lockstep -- an
unresolved frame reports `false`, never last frame's ticks), and the per-timed-pass kind is recorded at execute time
parallel to `m_pass_names`.

- **DX12:** `period = 1e9 / m_ts_freq` (ticks/sec -> ns/tick); `valid_bits = 64` (the D3D12 counter is a full 64-bit
  value); `pass_queue` = Graphics (this backend declares no async-compute queue).
- **Vulkan:** `period = m_ts_period` (already ns/tick); `valid_bits` is the **graphics queue family's real
  `timestampValidBits`**, cached at pool setup -- never assumed 64, because the (f) wrap-repair needs the true width.
  `pass_kind`/`pass_queue` come from the per-timed-pass record; `pass_queue` reports the **actual** queue (`on_async`,
  the graph's verdict, not the request). **Async passes report `pass_gpu_ticks == false`:** REN-8 writes both their
  timestamps on the GRAPHICS command buffer with the compute `fn` skipped between them, so the pair does not bracket
  the async work -- placing it would misattribute compute work to a graphics-timeline span; a true per-queue timing
  needs a compute-queue query pool (future work). The consumer counts such passes unavailable, honestly.

**Real-device verified (this box has a timestamp-capable GPU -- the REN-8 gates are NOT skipped here).** The device
timing tests were extended to reconstruct `pass_gpu_ms` from the raw ticks via a different code path and assert the
metadata; they RAN on real DX12 and Vulkan devices: `REN-8 GATE (DX12)` **32 assertions**, `REN-8 GATE (Vulkan)` **30
assertions**, both green -- `ms_from_ticks == pass_gpu_ms` within 1e-6, `period > 0`, valid-bits (DX12 == 64; Vulkan in
(0, 64]), `pass_kind == Raster`, `pass_queue == Graphics`, out-of-range -> `false`.

**Correction to (i)/(j):** "default-unavailable is the box's real verdict today" holds for the bridge's
`FakeFrameGraph` (which does not override the getters). The REAL backends now place passes, so on a real device a real
`FrameGraphGpuBackend` over a real frame graph emits placed samples and the (j) oracle flips incomparable -> match --
the end-to-end evidence (k) will cite (this box can run it).

**Gates.** DX12 + Vulkan compile clean under win-debug, win-shipping `/W4 /WX`, and `win-clang-cl-shipping` (0 warnings);
the extended REN-8 device gates pass on real hardware (DX12 32/1, Vulkan 30/1); the **full device suites pass with zero
regressions** (DX12 **10202/177**, Vulkan **6615/272**); the bridge tests (`FakeFrameGraph`, no backend dep) are
unchanged **57/12**; `crd-perf-tests` **1234/192** unchanged; container check PASS; both validators PASS. No repo-root
scratch (the device tests write `imgui.ini` to CWD -- deleted before the hygiene check).

**(k) implication of the async decision:** a workload with any async-compute pass is **incomparable** under (j) until
per-queue query pools exist (its stamps are graphics-timeline). (k) must therefore state, per acceptance clause, which
is satisfied by the REAL path (graphics passes: placed + compared) vs only the fake path (async producer signals).

Remaining: **(k)** the acceptance walk clause-by-clause against row 078 (below).

## (k) landed -- acceptance walk + flip (real end-to-end on this box's DX12 and Vulkan)

**The real end-to-end.** The REN-8 device gates (`test_dx12_frame_graph.cpp`, `test_vulkan_frame_graph.cpp`) now drive
their real, already-executed 2-pass frame graph through a real `crd::perf::gpu::FrameGraphGpuBackend` + `frame_mark` and
assert crd-perf placed both passes: track `gpu d0 q0` has 2 samples, `Category::Gpu`, `correlation_for(q0,i)->pass_id ==
intern_name("shadow_depth"/"shade")`, `device_id 0`, `queue_id kGpuQueueGraphics`, calibrated flag clear + sentinel
uncertainty, `uncalibrated_span_count()==2`, `total_match_count()==1`, `total_incomparable_count()==0`,
`gpu_timestamps_unavailable_count()==0`. Ran on real hardware (this box has a timestamp-capable GPU; SKIP -- never
pass -- when absent): **DX12 55/1, Vulkan 53/1**. A `PerfGuard` clears the backend before the bridge dies and shuts the
profiler down so the ~450 following device tests are unaffected.

**Acceptance clause verdicts** (verbatim acceptance in
[runtime-diagnostics.md#diag-6b](../design/runtime-diagnostics.md#diag-6b)):

| Clause | Verdict | Evidence / named follow-up |
| --- | --- | --- |
| Connect real frame-graph timestamps to crd-perf | **Satisfied (real)** | (i)+(i2)+(k): real DX12/Vulkan graph -> bridge -> placed samples, e2e 55/53 |
| Model execution + transfer separately | **Satisfied** | `GpuSampleKind` Execution->Gpu, Transfer->Io (c); CPU scopes/jobs are existing crd-perf |
| Model logical tasks + queue-wait separately | **Satisfied (crd-perf); producer future** | `GpuSampleKind::QueueWait` + jobs adapter exist; a submit-timestamp *producer* is future work (named) |
| Multiple queues / devices | **Satisfied (crd-perf)** | per-(device,queue) tracks proven by fake (c); on the REAL path no sample lands on a non-graphics track -- async passes report `pass_gpu_ticks == false` (their stamps sit on the graphics timeline), so `pass_queue` is reported but never *placed* until per-queue query pools exist (named); DX12 has one direct queue; multi-device untested (one GPU here) |
| Disjoint/unsupported clocks; calibrated else separate tracks | **Satisfied (else-branch, real)** | calibration mechanism (d) proven by fake; no CPU+GPU pair sampler exists, so the real path honestly takes "otherwise retain separate tracks" -- e2e asserts calibrated flag clear + sentinel |
| No submit-and-wait stalls for profiling | **Satisfied** | the bridge only reads already-resolved timings in `resolve_completed_frames`; it adds no fence/wait |
| Correct retired rhi examples; replace disconnected assembly | **Satisfied (real)** | (g) fixed the retired-rhi example; the bridge (i) replaces the "only a test mock implements IProfilerGpuBackend" gap with a real backend |
| Linked CPU/task/**pass** identities, DX12 + Vulkan | **Satisfied (real)** | e2e: `pass_id` linked to the authored names on both backends |
| Linked **resource** identity | **Satisfied (crd-perf); producer future** | `CorrelationRecord.resource_id` carried; the frame-graph producer sets only `pass_id` (a pass has N resources, the record one slot) -- named future work |
| Delayed query resolve | **Satisfied (crd-perf)** | fake proves crd-perf handling (f); the real backend resolves on fence-complete (REN-8), once per frame |
| Ring reuse | **Satisfied (crd-perf)** | fake proves `note_gpu_query_slot_reused` (f); the real backend does not reuse (passes past `kMaxTimedPasses` are unstamped) -- named |
| Timestamps unavailable | **Satisfied (crd-perf + real)** | fake proves counting (f); real: a Vulkan async pass / a pass past the cap reports unavailable. **Gap:** a real readback FAILURE -> `m_timed_passes=0` -> `pass_count()==0` -> the bridge's `n==0` guard counts nothing, not "unavailable", and `gpu_timing_available()` stays true. Fix shape: an `IFrameGraph` resolve-failure signal -- out of (k) scope, named |
| Wrap | **Satisfied (crd-perf)** | fake proves single-wrap repair (f); Vulkan's real `timestampValidBits` (i2) feeds it |
| Device loss | **Satisfied (crd-perf); producer future** | fake proves `note_gpu_device_lost` + pending-drop (f); the raster backend cannot call it (no perf dep) -- real device loss surfaces as the resolve-failure gap above; an interface-side signal is named future work |
| Compare total timing to an independent capture | **Satisfied (real)** | (j) oracle vs the graph's own `gpu_ms_total`; e2e asserts `total_match_count()==1` on real hardware |
| No cross-domain subtraction; no false failure-location precision | **Satisfied** | (j) compares duration-vs-duration in one domain, never `cpu_ns - gpu_ns`; a mismatch reports magnitude only, never which pass |
| "Existing authored workload" | **Satisfied (real, on the REN-8 test graph)** | the 2-pass timed graph on both backends; the sandbox app is not separately exercised -- named |

**Flip criterion met:** the e2e passes on **both** backends, and every clause is Satisfied-real or
Satisfied-crd-perf-with-a-named-follow-up. The named follow-ups (queue-wait/resource/device-loss *producers*;
**per-queue query pools** for real async-queue placement; the resolve-failure-vs-empty-frame signal) are
DIAG.7a-shaped provider-identity/validation work, not 6b gaps. Row 078 flipped **Open -> Needs CI**.

**Gates.** e2e (REN-8 filtered) DX12 55/1 + Vulkan 53/1 on real hardware; **full device suites re-run this tick
DX12 10225/177 + Vulkan 6638/272** (each +23 assertions vs i2's 10202 / 6615 -- the (k) e2e block lives inside REN-8,
so same case count), both green end-to-end -- so `perf::init`/`shutdown` behind the test's `PerfGuard` leaves the
~450 following device tests undisturbed (isolation **observed**, not assumed); bridge `FakeFrameGraph` 57/12;
`crd-perf-tests` 1234/192. **Device-test exes build clean under all three lanes** -- win-debug, win-shipping
`/W4 /WX` (perf on), and win-clang-cl-shipping `/W4 /WX` thin-LTO (perf off). The last needed a fix found this tick:
with the 8 perf headers `#include`d unconditionally, `lld-link` hit an access violation during thin-LTO codegen of the
frame-graph test function in the perf-OFF shipping lane. **Observed: unguarded fails, guarded passes.** Fix (test files
only): guard the heavy perf headers behind `#if CRD_PERF_ENABLED`, with the cheap `<crd/perf/config.hpp>` (build_config
+ types) included first so the gate is defined for both the guard and the e2e block. The fix is correct independent of
the ICE mechanism -- a config that never references those symbols should not pull their inline/template code into its
TU. container check PASS; both validators PASS; `imgui.ini` (device-test byproduct) removed before the hygiene check.
