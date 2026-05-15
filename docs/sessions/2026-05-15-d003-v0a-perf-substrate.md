# 2026-05-15 — Detour D-003 v0a: `crd-perf` substrate

## What shipped

**Detour D-003** opens 2026-05-15 (immediately after D-006 close). The
first slice **v0a** ships the profiler substrate skeleton:

**New module `engine/perf/` (5 headers + 1 cpp, ~700 LOC):**
- `config.hpp` — compile-time gate (`CRD_PERF_ENABLED`, wires to existing
  project-wide `CRD_ENABLE_PROFILING`) + sizing knobs: `kPerThreadRingSlots=4096`,
  `kMaxThreads=64`, `kMaxRegionNames=4096`, `kMaxGpuSpansPerFrame=256`,
  `kGpuFramesInFlight=4`, `kFrameHistorySlots=240`, `kMaxCounters=256`.
- `sample.hpp` — **32-byte `Sample` POD** (begin_ns 8 + end_ns 8 + name_id 4 +
  color 4 + begin_thread 1 + end_thread 1 + depth 1 + category 1 + fiber_id 4).
  `NameId` strong-typed handle + `Category` enum (User / Job / System / Pass /
  Render / Gpu / Memory / Io / Wait). On-disk format depends on this layout —
  field offsets pinned by static asserts + by offsetof tests.
- `profiler.hpp` — singleton API: `init` / `shutdown` / `is_active` /
  `intern_name` / `resolve_name` / `register_thread` / `current_thread_index` /
  `set_current_fiber_id` / `current_fiber_id` / `push_region` / `pop_region` /
  `frame_mark` / `frame_count` / `thread_samples` / `thread_count` /
  `clear_samples`. **`BeginToken`** (16 B) carries `begin_ns` + `begin_fiber` +
  `begin_thread` + `depth` from push to pop — the wire-format carrier that
  records fiber migration faithfully.
- `scope.hpp` — `ScopedRegion` RAII + `CRD_PERF_SCOPE("name")` /
  `CRD_PERF_SCOPE_CATEGORY("name", cat)` / `CRD_PERF_SCOPE_COLOR("name", rgba)` /
  `CRD_PERF_FRAME_MARK()`. Macros cache `NameId` in a TU-local static so
  intern_name runs once per call site.
- `perf.hpp` — umbrella.

**Implementation: `src/profiler.cpp` (~480 LOC).** Per-thread SPSC ring
(lock-free; relaxed-load head + release-store on push); content-keyed FNV-1a
linear-probe intern table (mutex-protected on insert, cold path only); thread
registration assigns indices 0…kMaxThreads-1 in `register_thread`; `t_ring` /
`t_thread_index` thread-locals cache the per-thread state on first push.

**Tests `tests/perf/` (9 files, 28 cases / 87 assertions):**
- `test_sample_pod.cpp` — layout pinned: sizeof==32, alignof==8, every field
  offset asserted (begin_ns@0, end_ns@8, name_id@16, color_rgba@20,
  begin_thread@24, end_thread@25, depth@26, category@27, fiber_id@28).
- `test_intern_names.cpp` — intern interns once, idempotent for identical
  literals, idempotent for content-equal but pointer-different strings,
  nullptr/inactive returns invalid, resolve(invalid) returns "" without crash.
- `test_scope_push_pop.cpp` — single + 3-deep nested + category + color +
  clear_samples-without-resetting-frame-count.
- `test_thread_registration.cpp` — main auto-registered, register idempotent,
  worker threads get unique indices, fiber id round-trip.
- `test_ring_overflow.cpp` — saturation at 16 slots + dropped counter +
  clear-and-refill semantics.
- `test_frame_mark.cpp` — frame_count starts 0, increments per mark, inactive
  is no-op.
- `test_fiber_migration.cpp` — non-migrated scope records begin==end thread;
  cross-thread pop records begin_thread!=end_thread (the v0c wire contract).
- `test_zero_overhead_gate.cpp` — inactive profiler accepts the macro without
  crashing; on/off compile-time gate verified.
- `test_determinism_contract.cpp` — **ADR-0063 substrate-level pin**:
  `deterministic_compute(10000)` produces bit-identical f64 with profiler
  inactive, profiler active+unwrapped, profiler active+CRD_PERF_SCOPE-wrapped
  every iteration. Profiling is observable-effect-free.

## Decisions locked at v0a

1. **Module renamed `crd-perf`** (was `crd-profiler` in original plan; renamed
   after advisor flagged collision with existing `crd-profile` quality-preset
   module — one-character distance = silent typo-elision risk across years
   of code). Macros: `CRD_PERF_SCOPE`, `CRD_PERF_FRAME_MARK`, etc.; namespace
   `crd::perf`.

2. **Fiber-migration wire format = paired Sample + (begin_thread, end_thread).**
   `push_region` returns a `BeginToken` capturing (begin_ns, begin_fiber,
   begin_thread, depth). `pop_region` consumes it and writes the Sample with
   both thread ids. Cross-thread pop → begin_thread != end_thread visible in
   the Sample. UI default = per-OS-thread tracks; per-fiber Tracy-style view
   reconstructable in v0c when JobObserver emits fiber-yield events.

3. **`CRD_PERF_ENABLED` reuses the existing project-wide `CRD_ENABLE_PROFILING`
   flag** — already wired per-preset (ON in win-debug/asan/relwithdebinfo/tidy/
   clang-cl; OFF in win-release/shipping). No new build switch.

4. **Sample is exactly 32 bytes** — on-disk capture format (v0f CPROF) memcpys
   Sample arrays verbatim. Any field reshuffle bumps the format version.

5. **Name interning = content-keyed FNV-1a + linear probe + insert-mutex**
   (cold path). Macro-side TU-local `static const NameId` cache keeps the
   hot path at one indirect-branch-predictable load after first hit.

6. **Per-thread SPSC ring, fixed slot count, lock-free push.** Overflow drops
   newest sample + bumps `dropped` counter (visible to UI as a red banner).
   Default 4096 slots = 128 KB per thread.

7. **C4324** (struct padding due to alignas) suppressed locally on
   `alignas(64) ThreadRing` — same pattern crd-jobs `ThreadState` uses.

8. **Auto-instrumentation hooks scoped to v0c** (advisor-driven scope
   expansion): jobs + scene Schedule systems + frame graph passes + RHI
   command-buffer regions, all behind a common `IInstrumentationSource`
   interface defined in v0a (deferred to v0c when first consumer wires up).

## Verification

| Config | Result |
|---|---|
| win-debug | **PASS** (1775/1775 full project ctest; 28/28 perf cases) |
| win-asan | **PASS** (28/28 perf cases) |
| win-shipping | **PASS** (3/3 perf cases; 22 cases compile out at the gate — verifies the off-path inert-substrate contract) |
| win-tidy | **PASS** (build clean) |

`crd-perf-tests` standalone: **87 assertions in 28 test cases**, all green.

## Issues encountered

1. **MSVC C4324** padding-after-alignas warning on `alignas(64) struct
   ThreadRing` — added local `#pragma warning(push/disable 4324/pop)` per
   the precedent in `crd-jobs::ThreadState`.
2. **MSVC C4996** `strcpy` deprecation in
   `test_intern_names.cpp` content-dedup test — replaced with
   `std::copy_n(src, len+1, buf)`.
3. **Initial `BeginToken` design returned `i64`** then was refactored mid-slice
   to a 16-byte POD capturing begin_thread + begin_fiber so fiber migration is
   faithfully recorded at the Sample level (the locked wire format from the
   v0a-design AskUserQuestion).

## What unlocks now

- **v0b** can start: typed counter substrate + per-frame snapshot ring.
- The substrate is consumable today by sandbox / smoke / engine modules —
  `CRD_PERF_SCOPE("name")` produces well-formed Samples that survive
  fiber migration.
- Determinism contract pinned at the substrate level — D-004 replay sandbox
  inherits it.

## Module count: +1

Cerid now has 27 leaf modules. `crd-perf` peer-substrate slots between
`crd-time` and `crd-memory` in the dependency graph.

## Next

**v0b — counters substrate + per-frame snapshot ring.** Typed
`Counter<i64/f64/Duration>` template + `CRD_PERF_COUNTER_*` macros +
rolling 240-frame snapshot history (the line-plot data source for v0g UI).
~400 LOC engine, ~250 LOC tests, 1 day.

Then: v0c auto-instrumentation (jobs + scene Schedule + frame graph + RHI),
v0d GPU timestamp backend, v0e memory tracking, v0f CPROF capture file
format, v0g ImGui frontend panels (the visualization), v0h sandbox +
ADR-0079 + full 17-config sweep close.
