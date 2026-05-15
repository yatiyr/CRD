# `crd-perf` + `crd-perf-ui` — perf measurement substrate

**Status:** v1 shipped 2026-05-15 (Detour D-003, slices v0a-v0h).
**ADR:** [ADR-0079](../decisions/0079-crd-perf-substrate-architecture.md).

## What it does

- **Region timing** — `CRD_PERF_SCOPE("name")` records begin/end
  timestamps into a 32-byte `Sample` POD on a per-thread SPSC ring.
- **Job auto-instrumentation** — `install_jobs_adapter()` wires
  `crd::jobs` so every job in the engine becomes a labeled
  `Category::Job` Sample with zero call-site code.
- **GPU timing** — `CRD_PERF_GPU_SCOPE(cmd_buffer, "name")` plus
  `create_vulkan_profiler_backend(device)` records `vkCmdWriteTimestamp`
  pairs and surfaces them on the "gpu" track.
- **Memory tracking** — `register_allocator(name, IAllocator*)` snapshots
  every allocator's `MemoryStats` into the FrameRecord history at every
  `frame_mark()`.
- **User counters** — `CRD_PERF_COUNTER_{SET,ADD}_{I64,F64,DURATION}`
  records typed values into the rolling 240-frame history.
- **Capture file format** — `save_capture_to_file(path)` writes a CPROF
  v1 blob; `CaptureView` loads + renders read-only.
- **ImGui frontend** — `ProfilerPanel` renders seven sub-panels
  (Frame Summary / Timeline / Flame Graph / Counters / GPU Passes /
  Memory / Capture Controls).

## Quick usage

```cpp
#include <crd/perf/perf.hpp>
#include <crd/perf/ui/ui.hpp>
#include <crd/rhi/vulkan_profiler_backend.hpp>

int main()
{
    // Bring-up
    crd::perf::init({});
    crd::perf::register_allocator("default", crd::memory::default_allocator());
    auto gpu_be = crd::rhi::create_vulkan_profiler_backend(*device);
    crd::perf::set_gpu_backend(gpu_be.get());
    crd::perf::install_jobs_adapter();
    crd::jobs::init({});

    // Frontend (inside an app that has crd-imgui attached)
    crd::perf::ui::ProfilerPanel panel;

    while (running)
    {
        // ... render frame; panel.draw() runs inside the ImGui frame ...
        CRD_PERF_SCOPE("frame.simulate");
        // ... CRD_PERF_GPU_SCOPE(cmd, "shadow_pass"); ...
        crd::perf::resolve_gpu_frames();
        crd::perf::frame_mark();
    }

    // Teardown (mirror)
    crd::perf::uninstall_jobs_adapter();
    crd::perf::set_gpu_backend(nullptr);
    gpu_be.reset();
    crd::perf::shutdown();
}
```

The sandbox does exactly this — see `sandbox/src/main.cpp`.

## Compile-time gate

Everything is gated on `CRD_ENABLE_PROFILING` (a project-wide
`#cmakedefine01`). When OFF, every `CRD_PERF_*` macro collapses to
`((void)0)` and the singleton is never built. Per-preset wiring:

| Preset | Gate | Notes |
|---|---|---|
| win-debug, win-asan, win-relwithdebinfo, win-tidy, win-clang-cl | **ON** | Default dev workflow |
| win-release, win-shipping | **OFF** | Consumer ship — zero overhead proven |
| **win-shipping-profile** | **ON** | New for D-003. Shipping LTCG + profiler — QA / playtest config |

The 5-config per-slice DoD (codified in
`feedback_per_slice_run_ctest.md`) requires every slice to pass
ctest on all five.

## Module graph

```
crd-core
  ├─ crd-time          (Instant + Duration + MonotonicClock + Stopwatch)
  ├─ crd-jobs          (Chase-Lev fibers; gains JobObserver hook)
  └─ crd-memory        (IAllocator + MemoryStats — gate widened in v0e)
        ↓
      crd-perf         (substrate: ring + intern + counters + frame history
                        + GPU surface + capture format + CaptureView)
        ↓
      crd-perf-ui      (ImGui frontend: IProfilerSource + ProfilerPanel)
        ↓
      crd-rhi-vulkan   (VulkanProfilerBackend implements IProfilerGpuBackend)
```

One-way; no cycles. `crd-jobs` does NOT depend on `crd-perf` — the
profiler subscribes via a function-pointer table at install time.

## Public API surface

### Substrate (`crd-perf`)

```cpp
// Lifecycle
void crd::perf::init(InitConfig);
void crd::perf::shutdown();
bool crd::perf::is_active();

// Region scopes
CRD_PERF_SCOPE(name_literal);
CRD_PERF_SCOPE_CATEGORY(name_literal, Category);
CRD_PERF_SCOPE_COLOR(name_literal, rgba);

// Frame
CRD_PERF_FRAME_MARK();
u64 crd::perf::frame_count();

// Counters
CRD_PERF_COUNTER_SET_I64(name, value);   // overwrite; survives frame
CRD_PERF_COUNTER_ADD_I64(name, delta);    // accumulate; reset by frame_mark
CRD_PERF_COUNTER_SET_F64(name, value);
CRD_PERF_COUNTER_ADD_F64(name, delta);
CRD_PERF_COUNTER_SET_DURATION(name, dur);
CRD_PERF_COUNTER_ADD_DURATION(name, dur);

// Thread registration
u8 crd::perf::register_thread(name);
u8 crd::perf::current_thread_index();

// GPU
crd::perf::set_gpu_backend(IProfilerGpuBackend*);
CRD_PERF_GPU_SCOPE(cmd_buffer_void_ptr, name);
crd::perf::resolve_gpu_frames();

// Jobs auto-instrument
crd::perf::install_jobs_adapter();
crd::perf::uninstall_jobs_adapter();

// Memory
u32 crd::perf::register_allocator(name, IAllocator*);
crd::perf::unregister_allocator(idx);

// Capture
Array<u8> crd::perf::save_capture_to_buffer(IAllocator*);
bool      crd::perf::save_capture_to_file(path, IAllocator*);
bool      crd::perf::validate_capture_buffer(span);

// Capture view (read-only)
class crd::perf::CaptureView;
```

### Frontend (`crd-perf-ui`)

```cpp
class crd::perf::ui::IProfilerSource;
class crd::perf::ui::LiveProfilerSource;
class crd::perf::ui::CaptureViewSource;

class crd::perf::ui::ProfilerPanel
{
    void set_source(IProfilerSource*);
    void draw();                                  // call inside ImGui frame
    bool save_capture_to_file(path);
    bool load_capture_from_file(path);
    // ... per-panel show toggles
};
```

## Key data shapes (CPROF-format-pinned)

| Type | Size | Notes |
|---|---|---|
| `Sample` | 32 B | begin_ns + end_ns + name_id + color + begin_thread + end_thread + depth + category + fiber_id |
| `BeginToken` | 16 B | RAII handoff; carries begin_thread + begin_fiber |
| `AllocatorRecord` | 48 B | alloc_count + dealloc_count + bytes_in_use + peak_bytes + total_bytes + pad |
| `FrameRecord` | 3616 B | frame_index + begin_ns + end_ns + counts + RawCounterValue[256] + AllocatorRecord[32] |
| `CprofHeader` | 72 B | magic + version + flags + captured_at_ns + counts + struct-size sanity |
| `ThreadHeader` | 56 B | thread_index + sample_count + sample_byte_offset + name[32] + dropped |
| `CounterMeta` | 64 B | index + kind + type + name[56] |
| `AllocatorMeta` | 64 B | index + name[56] |

All sizes static-asserted. Format change ⇒ bump `kCprofVersion`.

## What the panels show

| Panel | Data source |
|---|---|
| Frame Summary | Latest `FrameRecord` + top-N region aggregation across every thread |
| Timeline | Per-thread `Sample` arrays; zoom/pan; hover tooltip; **red border on fiber-migrated samples** |
| Flame Graph | Top-level region aggregation by `NameId`, sorted by total time |
| Counters | Table + per-counter mini line plot over the 240-frame history |
| GPU Passes | `Sample`s on the "gpu" track (auto-detected) |
| Memory | Allocator table + per-allocator `bytes_in_use` line plot |
| Capture Controls | Save / load / pause / clear / back-to-live |

## Sizing knobs (compile-time)

- `kPerThreadRingSlots = 4096` — 128 KB per thread
- `kMaxThreads = 64`
- `kMaxRegionNames = 4096`
- `kMaxCounters = 256`
- `kMaxAllocators = 32`
- `kFrameHistorySlots = 240` — 4 seconds at 60 fps
- `kMaxGpuSpansPerFrame = 256`, `kGpuFramesInFlight = 4`

Total static memory budget (with profiler init'd): ~1 MB per-thread
rings (64 × 16 KB) + ~870 KB frame history + ~16 KB counter table +
~20 KB allocator metadata + ~10 KB GPU query state = **~2 MB
fixed overhead** when the profiler is active.

## Determinism contract (ADR-0063)

Profiling **does not perturb deterministic computation**. The
`test_determinism_contract.cpp` test pins this: `deterministic_compute(10000)`
produces bit-identical f64 whether the profiler is inactive,
active+unwrapped, or active+`CRD_PERF_SCOPE`-wrapped every iteration.
The test re-runs as part of every slice's per-slice-check.

## Where to look next

- Quick start: this doc + `sandbox/src/main.cpp` (~10 lines of profiler
  wiring).
- Architecture: [ADR-0079](../decisions/0079-crd-perf-substrate-architecture.md).
- Session-by-session detail: `docs/sessions/2026-05-15-d003-v0*.md` (8 logs).
- Format spec: `engine/perf/include/crd/perf/capture.hpp` (struct
  definitions are the format spec).
