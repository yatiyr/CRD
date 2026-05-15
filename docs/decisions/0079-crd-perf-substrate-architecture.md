# ADR-0079 — `crd-perf` profiler substrate + ImGui frontend

**Status:** Accepted 2026-05-15 (D-003 close).

**Context.** Detour D-003 ships the engine's perf-measurement substrate.
Per the Strategic Execution Plan + the user-stated bar ("elite, very
understandable, ergonomic, top-quality visualizations, profile
everything in our system"), every CPU region, every job, every GPU
pass, every counter, every allocator must surface in a single tool;
captures must serialise to a file for sharing; the substrate must hold
under MSVC LTCG; instrumentation must be observable-effect-free under
ADR-0063 determinism contract.

This ADR captures the design decisions locked across v0a-v0g, all
verified under the 5-config per-slice-check (win-debug + win-asan +
win-shipping + win-shipping-profile + win-tidy) per
`feedback_per_slice_run_ctest.md`.

## Decisions

### D1. Module name `crd-perf` (not `crd-profiler`)

The original detour plan said `crd-profiler`. v0a opening question:
collision with the existing `crd-profile` (quality-preset) module —
one-character distance, silent typo-elision risk across years of code.
Renamed to `crd-perf`; macros are `CRD_PERF_SCOPE` / `CRD_PERF_COUNTER_*`
/ `CRD_PERF_GPU_SCOPE`; namespace is `crd::perf`.

### D2. Fiber-migration wire format = paired Sample + `(begin_thread, end_thread)`

Cerid uses Chase-Lev work-stealing fibers; a `ScopedRegion` whose
begin runs on thread A can end on thread B. Two wire-format options
considered:

- **Split BEGIN/END events** (Tracy model) — UI stitches at display
  time. Doubles wire size; complicates hot path; harder
  zero-overhead-when-off guarantee.
- **Paired Sample with both thread fields** — one 32-byte Sample
  carries `begin_thread` + `end_thread`. UI renders migrated scopes
  with a visual split (red border in v0g). v0c JobObserver also
  emits fiber-yield events so per-fiber Tracy-style tracks can be
  reconstructed from a capture.

Locked at v0a: **paired Sample**. UI default = per-OS-thread tracks;
per-fiber view = optional reconstruction.

### D3. Compile-time gate reuses existing `CRD_ENABLE_PROFILING`

No new build switch. `CRD_PERF_ENABLED == CRD_ENABLE_PROFILING`
(already per-preset wired in CMakePresets.json). When off, every
`CRD_PERF_*` macro expands to `((void)0)`; the singleton is never
constructed.

### D4. `Sample` is exactly 32 bytes, layout pinned by `static_assert`

The CPROF on-disk format memcpys `Sample[]` verbatim. Any field shift
bumps `kCprofVersion`. Field offsets pinned by `offsetof` tests at v0a.

### D5. Per-thread SPSC ring, lock-free push

Writer = recording thread; reader = snapshot path (frame_mark / capture
flush / UI panel). Acquire-load snapshot of head + tail. Overflow
drops newest + bumps `dropped` counter (red banner in UI).

### D6. ADR-0063 determinism contract — profiling is observable-effect-free

Profiling reads `MonotonicClock` and writes to per-thread rings but
does not perturb deterministic computation. v0a pins this via
`test_determinism_contract.cpp`: `deterministic_compute(10000)` produces
bit-identical f64 whether the profiler is inactive, active+unwrapped,
or active+`CRD_PERF_SCOPE`-wrapped every iteration.

### D7. Per-slice DoD = 5 configs (added 2026-05-15 in v0c)

Original plan was 4 configs. v0b post-discussion surfaced a real gap:
the gated code was only running under debug/asan optimisation; LTCG /
`/OPT:ICF` / `/O2` bugs in profiler hot paths would slip into
production. **Added `win-shipping-profile` preset** (= win-shipping +
`CRD_ENABLE_PROFILING=ON`). Per-slice DoD = win-debug + win-asan +
win-shipping (off-path inert) + **win-shipping-profile** (substrate
under LTCG) + win-tidy. Codified in
[[feedback_per_slice_run_ctest]] (4 → 5 configs).

### D8. Substrate observes substrate; no reverse module edges

The pattern across v0c (jobs) / v0d (rhi-vulkan) / v0e (memory):

- `crd-perf` PUBLIC-links its observation targets (`crd-jobs`,
  `crd-memory`).
- `crd-rhi-vulkan` PUBLIC-links `crd-perf` (implements
  `IProfilerGpuBackend`).

No module ever depends on `crd-perf-ui` upstream — the UI is the leaf.
`crd-jobs` does NOT depend on `crd-perf` (subscribes via
function-pointer table set by the profiler at install time).

### D9. Single-subscriber observer pattern

`crd::jobs::JobObserver`, `IProfilerGpuBackend`, `IProfilerSource` all
use single-pointer atomic subscription (acquire/release). Profiler is
the only consumer; multi-subscribe surface is YAGNI. Cost when no
observer: one nullptr load + branch-not-taken.

### D10. `MemoryStats` gate widened to `CRD_DEBUG || CRD_ENABLE_PROFILING`

v0e change to a foundational module. Previously stats tracking was
`CRD_DEBUG`-only — release builds had zero overhead but
`win-shipping-profile` showed zeros on the Memory panel. Widened the
gate so the profiler measurement config gets real numbers; consumer
ship configs (both gates off) still pay zero overhead.

### D11. CPROF file format — pinned POD layout, NameId-preserving, little-endian only

v0f locks the format:
- `CprofHeader` 72 B + `ThreadHeader` 56 B + `CounterMeta` 64 B +
  `AllocatorMeta` 64 B + NameBlob (sparse offset table preserving
  `Sample.name_id` across save/load) + `FrameRecord` 3616 B + `Sample`
  32 B. All sizes static-asserted; any change bumps `kCprofVersion`.
- Little-endian only. Cross-arch BE support is YAGNI for Cerid.
- One-way save policy: loaded captures live in a `CaptureView`, never
  overwrite live state (SPSC-ring race avoidance).

### D12. Polymorphic source interface for the UI

`IProfilerSource` (v0g) is the seam that lets every panel render
against either the live profiler (`LiveProfilerSource`) or a loaded
`CaptureView` (`CaptureViewSource`) through identical code. Replay /
diff / regression workflows fall out for free.

### D13. UI is a separate `crd-perf-ui` module

Substrate stays ImGui-free; frontend is swappable. `crd-perf-ui`
PUBLIC-links `crd-perf` + `crd-imgui`; vendor ImGui pulled via SYSTEM
PRIVATE (same pattern `crd-imgui` itself uses). **No `<imgui.h>` in
any public header** — consumers don't accidentally pull it into
engine code.

### D14. Deterministic per-name colors

`color_for_name(NameId) -> Color32` uses FNV-1a-style mix → HSV →
RGB32. Same `NameId` across runs always yields the same color so
users build muscle memory. Pinned in `panel_helpers.cpp`.

### D15. Sandbox wiring (v0h)

Boot sequence in `sandbox/src/main.cpp`:
```
perf::init({})
register_allocator("default (malloc)", default_allocator())
create_vulkan_profiler_backend(*device) + set_gpu_backend(...)
install_jobs_adapter()
push_overlay(ProfilerPanelLayer)       // calls panel.draw() inside ImGui frame
jobs::init(...)
```
Per-frame:
```
... render scene ...
resolve_gpu_frames()
frame_mark()
```
Teardown is the strict mirror.

## Slice ledger (v0a-v0h, all shipped 2026-05-15)

| Slice | Scope | Tests added |
|---|---|---|
| v0a | Per-thread SPSC ring + 32 B Sample POD + intern + ScopedRegion + macros + thread registration + frame_mark + fiber-migration wire + determinism + zero-overhead-gate | 28 cases / 87 assertions |
| v0b | Typed counters (Set/Add × i64/f64/Duration = 6 macros) + 240-slot FrameRecord history + multi-thread atomic CAS-loop f64 add | +25 cases / +63 assertions |
| v0c | `crd-jobs` JobObserver + `crd-perf` jobs adapter (every job → Category::Job Sample, zero call-site code) + new `win-shipping-profile` preset (per-slice DoD 4→5 configs) | +6 cases / +22 assertions |
| v0d | `IProfilerGpuBackend` in `crd-perf` (Vulkan-free) + `VulkanProfilerBackend` (VkQueryPool + non-blocking resolve) + `CRD_PERF_GPU_SCOPE` macro + "gpu" track via `emit_gpu_sample` external-write | +8 cases / +25 assertions |
| v0e | Widened `MemoryStats` gate + allocator registry + `AllocatorRecord[32]` in FrameRecord (size 2080 → 3616 B) | +9 cases / +36 assertions |
| v0f | CPROF v1 capture format + `CaptureView` read-only mirror of live profiler API | +7 cases / +47 assertions |
| v0g | `crd-perf-ui` ImGui frontend (`IProfilerSource` polymorphic seam + `ProfilerPanel` with 7 sub-panels + panel helpers) | +14 cases / +56 assertions |
| v0h | Sandbox wiring + ADR-0079 + `docs/systems/perf.md` + full 17-config sweep | smoke pass (529 frames @ 176 fps) |

**Total:** ~3800 LOC engine + ~2600 LOC tests across 8 slices; 97
test cases / 336 assertions across `crd-perf-tests` +
`crd-perf-ui-tests`; full project ctest 1830/1830 → 1844/1844 across
the detour.

## Consequences

- The engine has a production-quality profiler that runs the same in
  debug, ASan, and shipping-LTCG (with `CRD_ENABLE_PROFILING=ON`).
- Every job in the engine appears as a labeled `Category::Job` Sample
  with zero call-site code once `install_jobs_adapter()` runs.
- GPU profiling is a one-line install (`create_vulkan_profiler_backend`).
- Memory tracking is a one-line install per allocator.
- Captures share across machines via the CPROF format.
- The UI panel is a one-add layer in any ImGui-using app.
- The 5-config DoD catches LTCG-class bugs at slice close, not in
  production.
- `crd-units` v0a adoption (next phase) inherits the same 5-config
  protocol; every future substrate slice will run under
  `win-shipping-profile`.

## References

- `engine/perf/` — substrate (gate + sample + profiler + scope +
  counters + frame_record + gpu_scope + jobs_adapter + memory +
  capture + capture_view).
- `engine/perf-ui/` — ImGui frontend
  (profiler_source + capture_view_source + panel_helpers + profiler_panel).
- `engine/rhi-vulkan/.../vulkan_profiler_backend.{hpp,cpp}` — Vulkan
  GPU timestamp backend.
- `engine/jobs/include/crd/jobs/observer.hpp` + `src/observer.cpp` —
  jobs hook.
- `engine/memory/include/crd/memory/memory_stats.hpp` — widened gate.
- `tests/perf/` (14 files / 83 cases / 280 assertions),
  `tests/perf-ui/` (2 files / 14 cases / 56 assertions).
- `CMakePresets.json` — `win-shipping-profile` preset.
- Session logs: `docs/sessions/2026-05-15-d003-v0a/b/c/d/e/f/g/h-*.md`.
- Memory: `[[feedback_per_slice_run_ctest]]` (4 → 5 configs codified).
