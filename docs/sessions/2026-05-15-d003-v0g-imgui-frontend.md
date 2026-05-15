# 2026-05-15 — Detour D-003 v0g: ImGui frontend (`crd-perf-ui`)

## What shipped

The UX layer. New module `engine/perf-ui/` (separate target so the
substrate stays ImGui-free and the frontend is swappable):

### New module `engine/perf-ui/` (~1100 LOC)

**Public surface** (`include/crd/perf/ui/`):

- **`profiler_source.hpp`** — `IProfilerSource` abstract base + `LiveProfilerSource`.
  The polymorphic seam that lets every panel render against either the
  live profiler or a loaded `CaptureView` through identical code. 13
  virtual accessors covering: threads (count/name/samples/dropped/gpu-track),
  counters (count/info), allocators (count/info), frame history
  (count/record), name resolve, identity (source_label/is_live).
- **`capture_view_source.hpp`** — `CaptureViewSource` adapter wrapping a
  `crd::perf::CaptureView`. Implements every accessor by forwarding to
  the view; finds the gpu track by linear scan for the "gpu" thread name.
- **`panel_helpers.hpp`** — testable utility functions:
  - `color_for_name(NameId) -> Color32` — deterministic, evenly-distributed
    palette (FNV-1a-style mix → HSV → RGB32). Same NameId always yields
    the same color so users build muscle memory.
  - `color_for_category(Category)` — fixed Material-design-ish palette
    (User grey-blue, Job green, System amber, Pass blue, Render teal,
    Gpu purple, Memory pink, Io brown, Wait grey).
  - `format_duration(ns)` / `format_bytes(u64)` / `format_count(u64)` —
    human-readable formatting; picks the right scale (ns/us/ms/s,
    B/KB/MB/GB, /k/M/B).
  - `total_thread_duration_ns(samples)` — sum depth-0 durations.
  - `aggregate_top_level_by_name(samples)` — merge by NameId into
    `NameTotal { name, total_ns, occurrences }` for top-N panels.
- **`profiler_panel.hpp`** — `ProfilerPanel` orchestrator class.
  Constructed once after `crd::imgui::ImGuiLayer` is attached. Holds
  the `LiveProfilerSource` + optional `CaptureView`/`CaptureViewSource`
  pair for loaded captures. `draw()` renders all enabled sub-panels in
  one call. Per-panel toggles + capture state (recording paused, save/
  load path buffers, recent-captures list).
- **`ui.hpp`** — umbrella.

**Implementation** (`src/`):

- `live_source.cpp` — `LiveProfilerSource` delegates every accessor to
  the global `crd::perf::*` state.
- `panel_helpers.cpp` — pure functions; testable in isolation.
- **`profiler_panel.cpp`** — **the heart of v0g** (~800 LOC). Renders
  seven sub-panels:

  1. **Frame Summary** — source label (live/loaded), frame count, last
     frame's CPU duration + FPS readout, top-16 regions table
     (Name / Total time / Avg / Hits) aggregated across every thread.
  2. **Timeline** — Tracy-style horizontal track view. One row per
     thread; samples drawn as colored rectangles (per-name color or
     per-category fallback). **Fiber-migration captured visually**:
     samples where `begin_thread != end_thread` get a red border. Hover
     tooltip shows name + duration + thread + depth + fiber id +
     migration flag. Zoom buttons (`Fit`, `+2x`, `-2x`); middle-mouse
     drag pans the time axis.
  3. **Flame Graph** — top-level region aggregation across all threads;
     bars sized by fraction of total time, ordered descending; name +
     duration + percentage rendered on each bar.
  4. **Counters** — table (Name / Latest / Kind / Type) + per-counter
     mini line plot showing the rolling 240-frame history. Type-aware
     rendering: i64 = `%lld`, f64 = `%.3f`, Duration = formatted ns.
  5. **GPU Passes** — table of the last 64 GPU samples from the "gpu"
     thread track (auto-detected via `gpu_thread_index()`).
  6. **Memory** — allocator table (Name / In-use / Peak / Allocs /
     Deallocs with `format_bytes` / `format_count`) + per-allocator
     `bytes_in_use` line plot over the rolling history (in KB units).
  7. **Capture Controls** — recording pause toggle + clear-samples
     button + save-to-path + load-from-path + "Back to live" button.

- `ProfilerPanel::save_capture_to_file` calls
  `crd::perf::save_capture_to_file()`.
- `ProfilerPanel::load_capture_from_file` reads the file via `fopen_s`/
  `fread`, validates the buffer, constructs a `CaptureView` +
  `CaptureViewSource`, swaps the active source. **One-way:** loaded
  capture lives in the panel-owned buffer, never overwrites live state
  (per v0f locked policy).

### CMake module wiring

- `engine/perf-ui/CMakeLists.txt` — new target `crd-perf-ui`.
  PUBLIC-links `crd-core` + `crd-perf` + `crd-memory` + `crd-imgui`;
  pulls vendor ImGui headers via `SYSTEM PRIVATE` (same pattern as
  `crd-imgui` itself uses).
- Root `CMakeLists.txt` adds `engine/perf-ui` after `engine/imgui`
  (dependency order: `crd-perf-ui` needs `crd-imgui` declared first).

### Tests `tests/perf-ui/` (2 files, 14 cases / 56 assertions)

- **`test_panel_helpers.cpp`** (9 cases) — `color_for_name`
  determinism + alpha pin + dispersion (different ids → different
  colors); `color_for_category` palette pinned; `format_duration` /
  `format_bytes` / `format_count` scale boundaries; aggregation
  correctness (depth-0 filtering + name merging + out-capacity
  respect).
- **`test_profiler_sources.cpp`** (5 cases) —
  `LiveProfilerSource` forwards counts + resolves names; full save +
  `CaptureViewSource` round-trip with scopes/counter/allocator/frame_mark;
  `gpu_thread_index() == 0xFF` when no "gpu" track exists;
  `ProfilerPanel` defaults to live source + `set_source(nullptr)`
  resets to live.

## Design decisions locked at v0g

1. **Polymorphic source interface** (`IProfilerSource`). Live profiler
   and `CaptureView` render through identical panel code. Replay /
   diffing / regression workflows fall out for free (just swap the
   source pointer).
2. **Pure helper functions are testable; ImGui rendering is not.**
   The panel rendering itself lives entirely in `.cpp` files where it
   can include `<imgui.h>` privately. v0h sandbox integration
   exercises the rendering end-to-end visually; v0g tests cover the
   data-extraction helpers that drive what's drawn.
3. **Fiber-migration captured visually** via the red border on
   `begin_thread != end_thread` samples — the v0a-locked wire format
   pays off in the UI.
4. **Per-name colors are deterministic** (FNV-1a-style mix → HSV).
   Same `NameId` across runs always yields the same color; users
   build muscle memory.
5. **One-way capture load** — loaded captures never overwrite live
   state. The panel holds an owned buffer + `CaptureView` +
   `CaptureViewSource` triple; "Back to live" just resets the source
   pointer to the embedded `LiveProfilerSource`.
6. **No ImGui include in any public header** (`profiler_panel.hpp`
   uses `IProfilerSource&` + plain types). Consumers don't accidentally
   pull `<imgui.h>` into engine code.

## Verification (5-config per-slice DoD)

| Config | Result |
|---|---|
| win-debug | **PASS** (1844/1844 full project ctest; 14/14 perf-ui cases — 56 assertions) |
| win-asan | **PASS** (14/14 perf-ui cases — 56 assertions) |
| win-shipping | **PASS** (9/9 perf-ui cases — 33 assertions; 5 `CRD_PERF_ENABLED`-gated tests compile out) |
| win-shipping-profile | **PASS** (14/14 perf-ui cases — 56 assertions under LTCG + max optimization) |
| win-tidy | **PASS** (build clean after one isolate-declaration cleanup) |

## Issues encountered + fixed

1. **CounterId vs NameId mixup** in the round-trip test. Counter names
   live in the `CounterMeta` table, not the `NameBlob`; switched to
   `src.counter_info(cid.value).name`.
2. **`[[nodiscard]]` ignored** on `register_allocator` in a test;
   added `[[maybe_unused]] const auto aid = ...`.
3. **W4189** in `win-shipping` for `name_blob_start` (used only in a
   debug assert) — already fixed at v0f; no new occurrences in v0g.
4. **clang-tidy `readability-isolate-declaration`** on
   `crd::u64 t10 = 0, t20 = 0;` -- split into one declaration per line.

## What unlocks now

- **v0h** sandbox integration can begin: wire `ProfilerPanel` into the
  sandbox's ImGui layer; instrument a few sandbox systems with
  `CRD_PERF_SCOPE`; call `crd::perf::install_jobs_adapter` so jobs
  light up automatically; wire `create_vulkan_profiler_backend` so
  the "gpu" track populates from a real device. Plus ADR-0079 minting,
  `docs/systems/perf.md`, and the full 17-config sweep.
- The substrate + UX of D-003 are **complete in code**. Only the
  closing slice (v0h) remains.

## Next

**v0h — sandbox wiring + ADR-0079 + docs/systems/perf.md + full 17-config
`scripts/full-sweep.ps1` pass.** Wire `ProfilerPanel` as a Layer in
the sandbox, install jobs adapter + Vulkan GPU backend + register the
TLSF root allocator. Verify by running the sandbox and watching every
panel populate with real data. Mint ADR-0079 capturing the design
decisions across v0a-v0g. Write the system overview doc.
~250 LOC engine + docs + sweep, 1-2 days.
