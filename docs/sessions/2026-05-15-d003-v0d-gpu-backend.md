# 2026-05-15 — Detour D-003 v0d: `crd-perf` GPU timestamp backend

## What shipped

v0d wires GPU timing into the substrate. Two pieces:

### 1. Backend interface + scoped macro in `crd-perf`

**`engine/perf/include/crd/perf/gpu_scope.hpp` + `src/gpu_scope.cpp`** (~250 LOC):

- `GpuSpanHandle` — opaque per-span handle (Vulkan: index into a VkQueryPool
  slot pair).
- `ResolvedGpuSpan` — `{ NameId, begin_ticks, end_ticks }` POD returned
  by the backend after host readback.
- **`IProfilerGpuBackend`** abstract base:
  - `begin_frame(frame_index)` — bookmark a new frame slot in the ring.
  - `begin_span(cmd_buffer*, name) -> GpuSpanHandle` — record begin
    timestamp; cmd_buffer is opaque `void*` (Vulkan-free).
  - `end_span(cmd_buffer*, handle)` — record end timestamp.
  - `end_frame()` — bookkeeping after queue submit.
  - `resolve_completed_frames()` — non-blocking host readback; converts
    ticks → ns; pushes `Category::Gpu` Samples onto the "gpu" track.
  - `ns_per_tick()` — backend-cached calibration.
- `set_gpu_backend(*) / current_gpu_backend()` — single-subscriber pointer,
  acquire/release atomic. Mirrors the v0c `JobObserver` pattern.
- `gpu_thread_index()` — returns the profiler thread index that GPU
  samples land on (registered as "gpu" at backend install). UI reads this
  to drive the GPU timeline track.
- `emit_gpu_sample(Sample)` — backend-side helper. Stamps `category =
  Gpu` + `begin_thread = end_thread = gpu_thread_index` + writes
  directly into the gpu thread's ring via the new
  `detail::write_external_sample` path in `profiler.cpp` (the only
  legitimate "external sample write" — bypasses the thread-local hot
  path because the resolve thread isn't the gpu thread).
- `resolve_gpu_frames()` — drives the backend's resolve step from any
  thread; called from `frame_mark()`-like positions in user code.
- **`GpuScopedRegion` RAII** + **`CRD_PERF_GPU_SCOPE(cmd_void_ptr,
  "name")` macro**. Caches `NameId` in a TU-local static; nullptr-backend
  expansion is a clean no-op. When `CRD_PERF_ENABLED == 0` the macro
  collapses to `((void)0)`.

### 2. `VulkanProfilerBackend` in `crd-rhi-vulkan`

**`engine/rhi-vulkan/include/crd/rhi/vulkan_profiler_backend.hpp` +
`src/vulkan_profiler_backend.cpp`** (~280 LOC):

- `VulkanProfilerBackendDesc { max_spans_per_frame = 256, frames_in_flight = 4 }`.
- `create_vulkan_profiler_backend(Device&, desc) -> unique_ptr<IProfilerGpuBackend>`
  factory. Caller owns the lifetime + responsibility for
  `set_gpu_backend(get())` / `set_gpu_backend(nullptr)` before destruction.
- **Single VkQueryPool** sized `max_spans_per_frame * frames_in_flight * 2`
  (default = 256 × 4 × 2 = 2048 slots). Per-frame slot range:
  `[ frame_slot * slots_per_frame, frame_slot * slots_per_frame + slots_per_frame )`.
- `begin_frame(frame_index)`: book the current frame slot;
  reset `spans_used`; clear `span_names` cache; stamp `host_begin_ns =
  MonotonicClock::now()` so resolved samples can be anchored on the CPU
  timeline (proper CPU/GPU calibration is v0g UI work — for v0d we
  place every GPU span at `host_begin_ns` stretched by the resolved
  tick delta).
- `begin_span(cb, name)`: `vkCmdWriteTimestamp(cb,
  BOTTOM_OF_PIPE_BIT, pool, query_begin_at)` + record name + return
  span index. Overflow when `spans_used >= max_spans_per_frame` bumps
  an atomic counter and returns `kInvalidGpuSpan` — the matching
  `end_span` no-ops safely.
- `end_span(cb, handle)`: `vkCmdWriteTimestamp(cb, BOTTOM_OF_PIPE_BIT,
  pool, query_end_at)`.
- `resolve_completed_frames()`: walks every frame slot; non-blocking
  `vkGetQueryPoolResults(... VK_QUERY_RESULT_64_BIT)`. `VK_NOT_READY`
  skips and retries next call. Successful results converted via
  `ns_per_tick` (from `VkPhysicalDeviceLimits::timestampPeriod`) and
  fed to `emit_gpu_sample(...)`.
- **`vulkan_profiler_begin_frame(IProfilerGpuBackend&, CommandBuffer&,
  frame_index)`** convenience: forwards `begin_frame` AND emits
  `vkCmdResetQueryPool` on the supplied cmd buffer (the cmd-buffer
  reset is Vulkan-specific so it doesn't live on the abstract base).
- Asserts at startup if `timestampComputeAndGraphics == 0` (the device
  lacks timestamp support — rare, but caught loudly).

`crd-rhi-vulkan` now PUBLIC-links `crd-perf` (substrate-implements-substrate;
analogous to `crd-perf → crd-jobs` from v0c).

### 3. Profiler shutdown hook

`crd::perf::shutdown()` now clears the cached GPU backend pointer + gpu
thread index via a `detail::reset_gpu_state()` forward decl resolved at
link time. Without this, a subsequent `init()` would reuse a stale
thread index pointing into a freed thread table — caught by a flaky test
where fixture ordering left the index dangling.

## Tests `tests/perf/test_gpu_scope.cpp` (8 cases / 25 assertions)

Mock-backend driven (the real Vulkan backend needs a live device; that
gets exercised in the smoke binary + sandbox at v0h):

- `set_gpu_backend` round-trips; clear with nullptr.
- `set_gpu_backend` registers a "gpu" thread for resolved samples;
  shutdown clears the cached index.
- `GpuScopedRegion` calls `begin_span` + `end_span` on the backend.
- `CRD_PERF_GPU_SCOPE` macro produces matched begin/end via the backend.
- `emit_gpu_sample` lands on the gpu thread's ring with `category=Gpu` +
  `begin_thread == end_thread == gpu_thread_index`.
- `CRD_PERF_GPU_SCOPE` is a no-op when no backend is installed.
- `begin_frame` / `end_frame` lifecycle calls are forwarded.
- Backend reports `ns_per_tick`.

## Design decisions locked at v0d

1. **Backend interface in `crd-perf` (Vulkan-free), implementation in
   `crd-rhi-vulkan`.** `crd-perf` does not link Vulkan. The cmd buffer
   parameter is `void*` so the substrate stays backend-agnostic; the
   Vulkan implementation casts back to `crd::rhi::CommandBuffer*` and
   calls `vulkan_command_buffer()` to extract the VkCommandBuffer handle.
2. **One paired begin/end Sample per GPU span.** Same wire format as
   CPU regions; UI renders the gpu track exactly like a CPU thread track.
3. **`emit_gpu_sample` is the ONE legitimate external-thread write
   path.** All other code paths use the thread-local
   `push_region`/`pop_region`. The new `detail::write_external_sample`
   helper in `profiler.cpp` makes the cross-thread write explicit and
   constrained.
4. **`set_gpu_backend(nullptr)` does NOT clear `g_gpu_thread_index`.**
   By design — resolved samples that landed earlier remain queryable.
   Only `shutdown()` resets the index (it has to: the thread table is
   torn down).
5. **CPU/GPU clock calibration deferred to v0g UI.** v0d anchors every
   resolved span at `host_begin_ns` (the CPU timestamp at frame begin)
   and stretches by the GPU tick delta. UI will eventually do proper
   periodic calibration via paired `vkCmdWriteTimestamp` + `clock_gettime`
   sampling.
6. **Non-blocking host readback** (`vkGetQueryPoolResults` WITHOUT
   `VK_QUERY_RESULT_WAIT_BIT`). `VK_NOT_READY` retries next call;
   never stalls the CPU.
7. **Hardware overflow / wraparound is handled defensively.** If
   `end_ticks < begin_ticks` we drop the span rather than emit a
   garbage Sample. Overflow counters tracked but not yet UI-surfaced.

## Verification (5-config per-slice DoD)

| Config | Result |
|---|---|
| win-debug | **PASS** (1814/1814 full project ctest; 67/67 perf cases — 197 assertions) |
| win-asan | **PASS** (67/67 perf cases — 197 assertions) |
| win-shipping | **PASS** (6/6 perf cases — 23 assertions; 61 gated cases compile out at gate) |
| win-shipping-profile | **PASS** (67/67 perf cases — 197 assertions under LTCG + max optimization) |
| win-tidy | **PASS** (build clean) |

## Issues encountered + fixed

1. **GPU thread index leaks across fixture instances.** A test that
   uninstalled the backend left `g_gpu_thread_index` set; the next
   test seeing it != 0xFF was a fail. Fixed by clearing both the
   backend pointer and the cached index in `crd::perf::shutdown()`
   via `detail::reset_gpu_state()`.
2. **W4189 in `win-shipping`** (`const VkResult res = ...` unused
   because `CRD_ASSERT_MSG` compiles out at `NDEBUG`). Marked
   `[[maybe_unused]]`.

## What unlocks now

- **v0e** memory tracking can begin: `AllocatorRegistry` +
  `Profiler::register_allocator(name, IAllocator*)` + per-frame
  `AllocatorSnapshot`.
- Engine code that already has a `crd::rhi::CommandBuffer&` can sprinkle
  `CRD_PERF_GPU_SCOPE(&cb, "ShadowPass")` today; the real Vulkan
  backend will resolve them once `create_vulkan_profiler_backend()` is
  wired in the sandbox at v0h.

## Next

**v0e — memory tracking.** `AllocatorRegistry` + `register_allocator()`
+ per-frame `AllocatorSnapshot{name, alloc_count, dealloc_count,
bytes_in_use, peak_bytes}` (the `MemoryStats` data already exists on
every allocator — wire it into the profiler's frame-history pipeline).
Wire TLSF + GrowablePool + scene `ChunkAllocator`. ~300 LOC engine +
~200 LOC tests, 1-2 days.
