# 2026-05-15 — Detour D-006: `crd-time` substrate

## What shipped

**Detour D-006** opens + closes same day. Inserted into the Strategic
Execution Plan after user-prompted architectural pivot ("should the timer
be inside the profiler or a different module?"). Ships:

**New module `engine/time/` (8 headers + 2 cpps, ~600 LOC):**
- `duration.hpp` — `Duration = Quantity<dim::Time, f64>` alias. First major consumer of `crd-units` v0a.
- `instant.hpp` — `Instant` monotonic time point with `Instant ± Duration` arithmetic + `Instant - Instant → Duration`.
- `clocks.hpp` + `clocks.cpp` — `MonotonicClock` (steady_clock backed), `WallClock` (system_clock backed; Unix epoch), `CycleCounter` (rdtsc on x86, cntvct_el0 on ARM64) + `calibrate(Duration)` + `cycles_to_duration` helper.
- `stopwatch.hpp` — `Stopwatch` class (start/stop/elapsed/reset) + `ScopedStopwatch<Callback>` RAII variant + `CRD_TIME_SCOPED_STOPWATCH(callback)` / `CRD_TIME_SCOPED_STOPWATCH_AS(name, out_duration)` macros + drop-in compat methods (`elapsed_seconds()`, `elapsed_milliseconds()`, `elapsed_nanoseconds()`).
- `frame_clock.hpp` — `FrameClock` (variable-step + fixed-step accumulator + alpha-interpolation in [0, 1]). The "Glenn Fiedler / Gaffer-on-Games" integrator pattern. Constructor takes the fixed-step Duration (default 1/60 s). Methods: `tick()`, `delta()`, `total()`, `fixed_step_duration()`, `consume_fixed_step()`, `fixed_step_count()`, `alpha()`, `accumulator()`, `set_fixed_step_duration()`. Drop-in compat methods.
- `deterministic_clock.hpp` — `DeterministicClock` with integer-tick counter + fixed Duration tick period. Constexpr-evaluable. Used by D-004 replay + future networked lockstep + test/fuzz harnesses.
- `deadline.hpp` + `deadline.cpp` — `Deadline` (absolute time-point future + `from_now(Duration)` factory + `expired()` + `remaining()`) + `sleep_for(Duration)` + `sleep_until(Deadline)` + `yield_thread()`.
- `gpu_timestamp.hpp` — opaque `GpuTimestampHandle` + `GpuTimestampValues` (begin/end u64 tick pair) + `gpu_ticks_to_duration` / `gpu_timestamp_elapsed` helpers. **API surface only**; actual `vkCmdWriteTimestamp` capture lives in `crd-rhi-vulkan` + D-003 profiler.
- `platform_compat.hpp` — backward-compat aliases `crd::platform::Timer = crd::time::Stopwatch` + `crd::platform::FrameClock = crd::time::FrameClock` so existing consumers keep compiling during migration.
- `time.hpp` umbrella.

**Migration from `crd-platform`** (move-and-delete pattern, ADR-0076 §13 precedent):
- Deleted: `engine/platform/include/crd/platform/timer.hpp`, `engine/platform/src/timer.cpp`.
- Updated: `engine/platform/include/crd/platform/platform.hpp` umbrella now includes the compat shim instead of the deleted timer.hpp.
- Updated: `engine/platform/CMakeLists.txt` gained `crd-time` PUBLIC dep.
- Updated: `runtime/examples/smoke_frame_clock.cpp` + `tests/platform/test_timer.cpp` use the compat shim.

**Tests `tests/time/` (7 files, 53 cases, 102 assertions):**
- `test_instant_duration.cpp` — Duration arithmetic + Instant +/- Duration + UDL ingress (`1.5_s`, `1.0_ms`, `1.0_min`).
- `test_clocks.cpp` — MonotonicClock monotonicity + WallClock recent-year + CycleCounter calibration.
- `test_stopwatch.cpp` — start/stop/reset semantics + ScopedStopwatch callback + macro.
- `test_frame_clock.cpp` — first-tick zero-delta + configurable fixed-step + alpha in [0,1] + reset.
- `test_deterministic_clock.cpp` — `tick()`/`advance(N)` + bit-exact reproducibility + constexpr-evaluable.
- `test_deadline.cpp` — `from_now` + `expired()` + `remaining()` + `sleep_for` minimum sleep.
- `test_gpu_timestamp.cpp` — handle validity + tick→Duration conversion at different timestamp periods.

## Design decisions locked

1. **`Duration` is `Quantity<dim::Time, f64>`** — first major consumer of `crd-units` v0a; validates the substrate under a real downstream.
2. **`Instant` stores `i64 ns_since_epoch`** — ~292 years monotonic range. Cross-clock arithmetic (subtracting Instants from different clocks) is UB; pinned by convention, not type system.
3. **Three clock types serve distinct use cases**: Monotonic for intervals; Wall for log/save timestamps; CycleCounter for ~1 ns precision hot-path profiling.
4. **`FrameClock` ships fixed-step + alpha from day 1** — the form eylem v1c+ needs for ADR-0063 deterministic-by-construction physics.
5. **`DeterministicClock` is integer-tick** — bit-exact across compilers/SIMD/OSes. `elapsed()` is derived on demand from `tick_count * tick_period`, never accumulated as f64. Foundation for D-004 replay + networked lockstep.
6. **GPU timestamp API is delegation-only** — `crd-time` provides types + conversion helpers; actual `vkCmdWriteTimestamp` capture lives in `crd-rhi-vulkan` + D-003 profiler. Keeps platform/backend separation clean.
7. **Backward-compat shim** — `crd::platform::Timer` / `FrameClock` aliases survive via `platform_compat.hpp`. Existing consumers compile unchanged; new consumers use `crd::time::*` directly.

## Issues encountered

1. **`clock_t` symbol clash** with `<time.h>::clock_t`. My local `using clock_t = std::chrono::steady_clock;` shadowed the global. Fix: just use `std::chrono::steady_clock::now()` inline (no local alias).
2. **f64 conversion drift** — 60e9 ns × 1e-9 produces 60.0000000000000711 (1 ULP from literal 60.0). Tests use tolerance `< 1e-12` for any chained ratio operations.

## Verification

| Config | Result |
|---|---|
| win-debug | **PASS** (build + ctest) |
| win-asan | **PASS** (build + ctest) |
| win-shipping | **PASS** (build + ctest) |
| win-tidy | **PASS** (build) |

`crd-time-tests` standalone: **102 assertions in 53 test cases, all green**.

Full project ctest 1645+ (crd-time adds 53 cases). All shipped binaries (smoke_frame_clock + test_timer + every other consumer) compile cleanly through the compat shim.

`crd-no-untagged-physical-numeric` guard: **PASS** (no offenders — `crd-time` ships dimensional from day 1).

## What unlocks now

- **D-003 profiler** can begin — `crd-time::Stopwatch` + `ScopedStopwatch` + `CycleCounter` + `GpuTimestampHandle` ready to use.
- **D-004 replay sandbox** can begin — `DeterministicClock` ready.
- **D-005 config/resource hot-reload polish** unblocked (parallel; doesn't depend on D-006 directly).
- **`crd-units` substrate has its first major real-world consumer** — `Duration` flowing through `Instant` arithmetic, `FrameClock` accumulator, `DeterministicClock` integer-tick × Duration math. Validates the v0a substrate under load.
- **Future eylem v1c+ fixed-step** ships consuming `crd-time::FrameClock` + `DeterministicClock` natively (no narrow internal clock-then-refactor pattern).

## Module count: +1

Cerid now has 26 leaf modules. `crd-platform` lost the timer surface (now only Window/Input/OS/Filesystem/DynLib/Threading/FileWatcher); `crd-time` peer-substrate fills the slot.

## Next

**D-003 profiler dashboard** — consumes `crd-time` for sample capture + ImGui flame graph + GPU timing integration via `vkCmdWriteTimestamp` (the `gpu_timestamp.hpp` API surface). Estimated ~2 weeks per the Strategic Execution Plan.

In parallel: D-004 replay sandbox (uses `DeterministicClock`), D-005 config/resource hot-reload polish.
