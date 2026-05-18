# 2026-05-18 — Phase 3.1.7 v9a-a follow-ons (all 4) ✅ SHIPPED

**Slice:** v9a-a-typed + v9a-60bit-cpu + v9a-a-async-compute + v9a-60bit-gpu. **Combined "v9a-a follow-ons" close.** Same day as the v9a-a base slice. Filed-but-deferred entries from earlier this day were paid in full per user direction.

**Status:** ✅ shipped same day. 5-config DoD PASS via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel`.

---

## The decision reversal

At v9a-a close I (claude) filed three follow-ons as deferred per the "ship at consumer" rule. User pushed back: **"We are building the engine substrate; ship it fully. Tests are cheap with the v9-prereq-test-harness; this is the time."** Acknowledged the rule was misapplied to substrate work — substrate IS the product, so settled-design enhancements with cheap tests should ship proactively. Memory entry `feedback_ship_at_consumer_template_from_day_one` refined accordingly (distinguishing question: "does the design have unsettled tradeoffs only a consumer can resolve?"; YES → defer; NO → ship).

Order shipped:
1. **v9a-a-typed** (~half day, ~50 LOC + ~150 LOC tests)
2. **v9a-60bit-cpu** (~1 day, ~150 LOC + ~150 LOC tests)
3. **v9a-a-async-compute** (~2 days, ~250 LOC + ~80 LOC tests)
4. **v9a-60bit-gpu** (~1 day, ~200 LOC + ~150 LOC tests)

---

## v9a-a-typed — Length<T> AABB wrapper

New header `engine/geometry-bvh-gpu/include/crd/geometry/bvh_gpu/morton_typed.hpp`. Strip-compute-retag per ADR-0078 §5 D34.

- `AABB3T<D, T>` typed AABB (min/max carry Dim tag).
- `compute_morton_codes_cpu_typed<D, T>(...)` — typed input, strip to raw, dispatch through raw entry.
- `dispatch_morton_codes_typed<D, T>(...)` — typed GPU dispatch.

Morton codes are dimensionless (bit indices, not lengths) → output stays `Array<u32>`.

**Test contract**: byte-identical to raw entry given equivalent input. 3 tests / 16 assertions across CPU (both with-scene and union-scene overloads) and GPU. ValidationCapture silent.

Module gains `crd-units` PUBLIC link.

---

## v9a-60bit-cpu — u64 60-bit Morton CPU oracle

New header `morton_60bit.hpp` + impl `src/morton_60bit.cpp`. Mirror of 30-bit path, scaled to 20 bits per axis (~1M³ resolution per axis).

- `spread_bits_60(u64)` — 5-step magic-mask cascade for u64.
- `morton3_60bit_from_ints` — combine three 20-bit ints.
- `quantize_to_morton_grid_20bit` — world → 20-bit bin.
- `compute_morton_codes_cpu_60bit` batch driver (with-scene + union-scene overloads).

**CALIBRATION FIRST corpus** (7 tests / 59 assertions): lane-pinned `spread_bits_60` bit patterns + canonical `morton3_60bit_from_ints` triples + `quantize_to_morton_grid_20bit` endpoints/clamping.

**Discriminating test**: km-scale scene where 30-bit collides (1m/bin) but 60-bit resolves (~1mm/bin). Two AABBs at 0.5m apart in a 1km scene:
- 30-bit: `codes[0] == codes[1]` (collide)
- 60-bit: `codes[0] != codes[1]` (resolve)

Locks in CAM / orbital scale support as a first-class shipping target.

**One mid-slice bug fixed**: my initial expected value for `spread_bits_60(0xFFFFF)` was `0x1249249249249249` (had a phantom bit at position 60); actual output is `0x0249249249249249` (bits 0,3,6,...,57 = exactly 20 bits = correct). Test expectation fixed.

---

## v9a-a-async-compute — RHI compute-family pool + async dispatch path

**RHI surface change** (appended at END per D135 vtable-stability):
- `Device::create_command_buffer_for_queue(Queue&)` — allocates from the command pool matching the passed queue's family. Pointer-identity routing per D9 contract: `&queue == &graphics_queue()` → graphics pool; `&queue == &compute_queue()` → compute-family pool (if dedicated) else graphics pool fallback.

**Vulkan backend impl** (`engine/rhi-vulkan/src/vulkan_backend.cpp`):
- Added `m_compute_command_pool` field. Lazy-allocated at device init when `m_compute_family_index != UINT32_MAX && != graphics_family_index`.
- Destroyed in dtor.
- `create_command_buffer_for_queue` override routes by pointer identity.

**`MortonGpuPipeline::dispatch_morton_codes_async`** opt-in async path:
- Same dispatch body as sync path; only the queue + cmd-buffer factory differ.
- Both paths fence-waited (currently SYNCHRONOUS — true non-blocking is a future enhancement when a consumer wants overlap).
- Refactored dispatch body into a free function `dispatch_inner(impl, ..., use_async)` shared between sync + async; `MortonGpuPipeline::Impl` promoted to public (forward decl in header; definition stays in .cpp) so the free function can access it (same pattern as `ValidationCapture::Impl`).

**Test contract**: cross-queue-family submit on a GPU with a dedicated compute family (RTX 4070 Ti SUPER here) must (a) produce byte-identical output to sync path, (b) leave ValidationCapture silent (the VUID-vkQueueSubmit-pCommandBuffers-00074 bug that bit us at v9a-a is the discriminating test). 1 test / 9 assertions; passed.

---

## v9a-60bit-gpu — u64 60-bit GPU dispatch + shaderInt64 gate

**RHI surface change** (appended at END per D135):
- `Device::supports_shader_int64()` — capability accessor for the Vulkan core 1.0 `shaderInt64` feature.

**Vulkan backend impl**:
- Reads `shaderInt64` from `vkGetPhysicalDeviceFeatures2` during adapter probe.
- Enables it via `enabled_features2.features.shaderInt64 = supported ? VK_TRUE : VK_FALSE` at device create.
- Threads through `VulkanDevice` ctor as `bool shader_int64_enabled` field.

**GLSL shader** `runtime/examples/shaders/compute_morton_codes_60bit.comp`:
- `#extension GL_ARB_gpu_shader_int64 : require` to enable `uint64_t`.
- Mechanical translation of CPU 60-bit kernel body (D134 discipline).
- Storage buffer output is `uint64_t codes[]`.

**`MortonGpu60BitPipeline`** — sibling pipeline class with same shape as `MortonGpuPipeline`:
- Ctor checks `Device::supports_shader_int64()` → returns `is_valid() == false` if unavailable (graceful degradation).
- `dispatch_morton_codes_60bit` returns `Array<std::uint64_t>`.
- Caches pipeline state across dispatches (same lifecycle pattern as 30-bit).

**Tests** (2 tests / 12 assertions):
- Capability test: ctor's is_valid() matches `device->supports_shader_int64()` (gracefully skips if unavailable).
- bit_compare<u64> test: 1024 random AABBs → CPU 60-bit oracle vs GPU 60-bit dispatch must be byte-identical. ValidationCapture silent.

---

## Combined test corpus totals

- **v9a-a base + 4 follow-ons**: 26 tests / 175 assertions in `tests/geometry-bvh-gpu/`.
- All v9-prereq-test-harness discipline preserved: ValidationCapture wraps every GPU test; bit_compare for byte-identity vs CPU oracle; gpu_determinism_check 3 rounds where applicable; CRD_PERF_BUDGET_LE for kernel cost.
- 5-config DoD via `-IncludeRelease` (mandatory for every v9a slice per `feedback_v9_gpu_sanity_harness`).

---

## Pinned design decisions (additions to ADR-0076 §25 amendment at v9a-close)

- **D137 (v9a-a-typed)** — `morton_typed.hpp` strip-compute-retag wrappers ship at v9a-a in-line. CPU+GPU entries; output codes stay raw `u32` (dimensionless bit indices).
- **D138 (v9a-60bit-cpu)** — u64 60-bit Morton path ships at v9a-a in-line. 20 bits per axis. CPU oracle defines the algorithm (D134 discipline scales).
- **D139 (v9a-a-async-compute)** — `Device::create_command_buffer_for_queue(Queue&)` virtual appended (D135-compliant). Routes by pointer-identity per D9. Vulkan backend lazy-creates a compute-family `VkCommandPool` when a dedicated compute family exists.
- **D140 (v9a-60bit-gpu)** — `Device::supports_shader_int64()` virtual appended. `MortonGpu60BitPipeline` graceful-degrades when feature unavailable. CPU 60-bit oracle still works without GPU (the v9a-60bit-cpu slice).

D136 (the "defer follow-ons" pin) was REVISED earlier this day — D137-D140 represent the paid-in-full state.

---

## Files touched

**New module files in `engine/geometry-bvh-gpu/`:**
- `include/crd/geometry/bvh_gpu/morton_typed.hpp` (v9a-a-typed)
- `include/crd/geometry/bvh_gpu/morton_60bit.hpp` (v9a-60bit-cpu)
- `src/morton_60bit.cpp` (v9a-60bit-cpu)
- `src/dispatch_60bit.cpp` (v9a-60bit-gpu)
- `include/crd/geometry/bvh_gpu/dispatch.hpp` extended with `dispatch_morton_codes_async` + `MortonGpu60BitPipeline`
- `src/dispatch.cpp` refactored: dispatch body in free `dispatch_inner(impl, ..., use_async)`; sync + async public entries both route through it
- `CMakeLists.txt` adds `crd-units` to PUBLIC deps

**New tests in `tests/geometry-bvh-gpu/`:**
- `test_morton_typed.cpp` (v9a-a-typed)
- `test_morton_60bit_cpu.cpp` (v9a-60bit-cpu)
- `test_morton_60bit_gpu.cpp` (v9a-60bit-gpu)
- `test_morton.cpp` gains `[async]` test case (v9a-a-async-compute)
- `CMakeLists.txt` adds `crd-units` link + the new 60bit shader to compile list

**New GLSL shader:**
- `runtime/examples/shaders/compute_morton_codes_60bit.comp`

**RHI surface extensions:**
- `engine/rhi/include/crd/rhi/device.hpp` — two new virtuals appended at END:
  - `create_command_buffer_for_queue(Queue&)`
  - `supports_shader_int64() const noexcept`
- `engine/rhi-vulkan/src/vulkan_backend.cpp` — corresponding overrides + `m_compute_command_pool` field + `m_shader_int64_enabled` field + `shaderInt64` feature probe & enable + `VK_KHR_surface` instance extension fix (carried from v9a-a base)

**Docs synced:**
- `docs/debt.md` — v9a-a follow-on entry updated mid-flight to reflect the in-progress reversal; will be marked CLOSED post-sweep.
- `docs/phases/phase-3.1.7-geometry.md` — 4 new slice rows between v9a-a and v9a-b1.
- `docs/systems/geometry-bvh-gpu.md` — follow-ons section restructured to ship-in-line + D136 revision noted.
- `context.md` — Last shipped milestone updated with the 4-follow-on plan.
- `MEMORY.md` + `feedback_ship_at_consumer_template_from_day_one.md` — rule refined post-pushback.

## Next

🎯 **v9a-b1 CPU radix sort** — deterministic `(morton_code, original_index)` pair sort. **Now templated over `KeyT` from day 1** with BOTH `KeyT=u32` (30-bit) AND `KeyT=u64` (60-bit) instantiated and tested, since the 60-bit CPU+GPU paths are now real shipping code. The forward-compat plan from earlier this day is now a tested reality, not a hypothetical hook.
