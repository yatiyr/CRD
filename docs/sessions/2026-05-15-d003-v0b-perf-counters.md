# 2026-05-15 — Detour D-003 v0b: `crd-perf` counters + frame snapshot ring

## What shipped

**v0b** ships the typed counter substrate + the rolling frame-history
ring that v0g UI line-plots will read.

**Extends `engine/perf/` (+2 headers, ~310 LOC engine + ~510 LOC tests):**
- `counters.hpp` — `CounterKind` (Set | Add) + `CounterType` (I64 | F64 |
  DurationNs) + `CounterId` strong handle; cold-path registration
  (`register_counter_i64` / `_f64` / `_duration`); hot-path writes
  (`counter_set_*` / `counter_add_*`); introspection (`counter_count`,
  `counter_info`, `counter_current_*`); frame-history accessors
  (`frame_record`, `frame_record_count`); 6 macros
  (`CRD_PERF_COUNTER_SET_I64/F64/DURATION` +
  `CRD_PERF_COUNTER_ADD_I64/F64/DURATION`). Off-gate stubs collapse to
  `((void)0)`.
- `frame_record.hpp` — `RawCounterValue` (8 B u64-bits) + `FrameRecord`
  (frame_index 8 + frame_begin_ns 8 + frame_end_ns 8 + counter_count 4 +
  pad 4 + `RawCounterValue[kMaxCounters]` 2048 = **2080 B**). Pinned by
  static_assert.

**Profiler singleton extended (`src/profiler.cpp`):**
- `CounterEntry` (name + kind + type + `std::atomic<u64> bits`). Fixed-size
  array of `kMaxCounters` slots; `counter_count_atomic` monotonically grows.
- Registration dedups by **(name, kind, type)** triple. Same name + Set + I64
  vs same name + Add + I64 are distinct ids — a user mistake worth surfacing
  rather than silently aliasing.
- `frame_history` ring (`FrameRecord[frame_history_slots]`, default 240),
  `frame_history_head` atomic counter, `last_frame_end_ns` for chaining.
- `frame_mark()` now snapshots every registered counter into the head
  FrameRecord, resets Add-kind counters to zero, and advances head. Set-kind
  counters survive (overwrite-last-wins semantics).
- f64 writes use bit_cast through u64; Add-kind f64 uses a CAS-loop (most
  ISAs don't have lock-free f64 fetch_add). All ordering relaxed — counters
  are an "approximate at frame boundary" surface, not a happens-before
  primitive.

**Tests `tests/perf/` (+5 files, 25 cases / 63 assertions):**
- `test_counters_register.cpp` (5 cases): valid handle round-trip; dedup
  by name+kind+type; differ-by-kind (Set vs Add) → distinct ids;
  differ-by-type (I64 vs F64 vs Duration) → distinct ids; inactive
  profiler returns invalid.
- `test_counters_set_add.cpp` (8 cases): Set-i64 overwrites; Add-i64
  accumulates; Set-f64 round-trip; Add-f64 accumulates via CAS-loop;
  Set-Duration round-trip with `_s` / `_ms` UDLs; Add-Duration
  accumulates; invalid id writes are no-ops.
- `test_counters_frame_snapshot.cpp` (7 cases): frame_mark captures
  FrameRecord; Set survives; Add resets; `frame_record(N)` reaches into
  ring; ring saturates at slot count (verified with 4-slot ring + 10
  frames); frame_begin_ns chains to previous frame_end_ns; f64
  round-trips through the ring.
- `test_counters_macros.cpp` (4 cases): macros register once and write
  thereafter; ADD macro accumulates across calls; SET/ADD_DURATION
  macros work; macro caches CounterId in TU-local static (1000 calls →
  one registration).
- `test_counters_threaded.cpp` (2 cases): 4 threads × 25,000
  `counter_add_i64` = exact 100,000 (atomic fetch_add); 4 threads ×
  5,000 `counter_add_f64(0.001)` = 20.0 ± 1e-6 (CAS-loop converges
  under contention).

## Design decisions locked at v0b

1. **Two kinds, three types.** `Set` (overwrite-last-wins; value survives
   frame_mark) + `Add` (accumulate-within-frame; reset to 0 by
   frame_mark). Three types: `i64` / `f64` / `Duration` (stored as `i64`
   ns ticks for serialization). Min / Max kinds deferred — not needed
   for v0g UI.
2. **Dedup by (name, kind, type) triple, not just name.** Catches the
   "same logical name registered with different semantics in two TUs"
   user error by giving distinct ids — UI surfaces them separately.
3. **FrameRecord is fixed-size POD = 2080 B; pinned by static_assert.**
   The on-disk CPROF format (v0f) memcpys these records verbatim. Any
   layout change bumps the format version.
4. **All counter writes are atomic-relaxed.** Safe from any thread
   (jobs / GPU resolve threads / scene Schedule systems). The frame_mark
   snapshot reads relaxed too — counters are "approximately at this
   frame boundary," not strict-happens-before. Strict ordering would
   require a barrier per write — way too expensive for the
   counter-bump-in-a-tight-loop use case.
5. **f64 Add uses CAS-loop** because most ISAs lack lock-free
   `fetch_add<f64>`. Relaxed CAS keeps the cost at one extra cache-line
   read under contention.
6. **InitConfig.frame_history_slots** now wired (was reserved at v0a).
   Default 240 frames = 4 seconds at 60 fps; plenty for UI line plots.
7. **kMaxCounters = 256** fixed at v0a. FrameRecord pre-allocates the
   full slot array so reads are O(1) regardless of how many counters
   are registered. Adding a counter past 256 asserts.

## Verification

| Config | Result |
|---|---|
| win-debug | **PASS** (1800/1800 full project ctest; 53/53 perf cases — 150 assertions) |
| win-asan | **PASS** (53/53 perf cases — 150 assertions) |
| win-shipping | **PASS** (6/6 — all v0b cases gated under `#if CRD_PERF_ENABLED`, compile out as expected; layout-pinning tests survive) |
| win-tidy | **PASS** (build clean after one tidy hint about `3.14159` → `std::numbers::pi` resolved by switching to `1.25`) |

`crd-perf-tests` standalone: **150 assertions in 53 test cases**, all green.

## Issues encountered

1. **`Catch::Approx` missing include** — needs `<catch2/catch_approx.hpp>`
   explicitly; not pulled in by `catch_test_macros.hpp`. Added in 3 files.
2. **clang-tidy `modernize-use-std-numbers`** flagged `3.14159` in the
   F64 Set round-trip test. Switched to `1.25` (not pi-shaped); the
   test's intent is "f64 round-trips bit-exact," not "store pi."

## What unlocks now

- **v0c can begin: auto-instrumentation hooks.** `crd-jobs` JobObserver
  hook into the scheduler; profiler subscribes at init to capture
  per-job begin/end + fiber-yield events. Plus the same pattern for
  scene `Schedule` system regions + frame-graph pass regions + RHI
  command-buffer regions, all behind the `IInstrumentationSource`
  interface scoped at v0a.
- v0g UI now has a complete data source for counter line-plots: it
  walks `frame_record(N)` from N=0 (most recent) up to
  `frame_record_count()-1` and decodes `values[i].bits` by
  `counter_info(i).type`.

## Next

**v0c — auto-instrumentation hooks (jobs + scene Schedule + frame graph +
RHI cmd-buffer regions).** The "profile everything" pin. Single
`IInstrumentationSource` interface + per-substrate adapter. Adds the
crd-jobs PRIVATE callback edge (no module dep from jobs to perf — the
profiler subscribes via a function pointer set at init). ~500 LOC engine
+ ~300 LOC tests, 2-3 days.
