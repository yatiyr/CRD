# 2026-05-18 — Phase 3.1.7 v9-prereq-test-harness ✅ SHIPPED

**Slice:** Phase 3.1.7 v9-prereq-test-harness — GPU sanity-check infrastructure for the 16 v9 GPU geometry slices.

**Status:** ✅ shipped same day. 5-config DoD PASS (`scripts/per-slice-check.ps1 -IncludeRelease -Parallel`) — win-debug + win-asan + win-shipping + **win-release** + win-tidy.

---

## Motivation

The 16 upcoming v9 GPU geometry slices (LBVH, BVH refit, V-HACD voxelize, V-HACD decompose, shader-helpers ULP, …) all hit the same sanity wall: how do you tell "the GPU did what the CPU does" from "the GPU race-conditioned the answer in a way that happens to be valid sometimes"? The discriminating tests are:

1. **Did Vulkan validation fire?** (the authoritative oracle for "did I drive Vulkan correctly")
2. **Does the GPU output match the CPU reference within an ULP?** (numerical conformance)
3. **Is the GPU output byte-identical across N runs?** (determinism for slices that claim it)
4. **Did the dispatch hit its published per-kernel budget?** (regression guard for the 8-ms LBVH target)

Building these one-off at every v9 slice would balloon test code and leave gaps. Building them once, here, before v9c-a starts means every v9 slice consumes one set of helpers — and the discipline lands at the start instead of leaking in over time.

---

## The 5 deliverables

### 1. `crd::rhi::ValidationCapture` (RAII per-instance)

**Header:** `engine/rhi-vulkan/include/crd/rhi/vulkan_validation_capture.hpp`
**Impl:** `engine/rhi-vulkan/src/vulkan_validation_capture.cpp`

Installs a **second** `vkCreateDebugUtilsMessengerEXT` messenger on the live `VkInstance` (peer to the engine's own log-sink messenger; the Vulkan layer fan-outs to all registered messengers). Atomic `std::atomic<u32>` per-severity counters. Mutex-protected records buffer capped at 256 messages with a drop count signal. VUID whitelist for known-benign messages (still recorded, not counted).

```cpp
auto capture = crd::rhi::ValidationCapture(*instance);
dispatch_lbvh_build(...);
device.wait_idle();
REQUIRE(capture.error_count()   == 0);
REQUIRE(capture.warning_count() == 0);
```

Lifetime: ctor installs, dtor removes. Multiple captures on the same instance are supported (each is independent). Thread-safe (callback may fire from any driver thread). Falls back to silent observation when validation isn't enabled on the instance — tests stay green-but-meaningless rather than crashing.

### 2. `crd::test::ulp_compare<float>` + `crd::test::bit_compare<T>`

**Header:** `tests/test_helpers/include/crd/test_helpers/gpu_compare.hpp`

`ulp_compare`: Bruce Dawson signed-magnitude → two's-complement bit-twiddle for f32 ULP distance. Handles +0/-0 as equal. Rejects NaN (always returns max). Reports first-mismatch index + CPU value + GPU value + ULP delta + count compared, so test failures triage with one INFO line.

`bit_compare<T>`: SFINAE'd to integral types. Pure `memcmp`-class compare; the ULP field stays 0.

```cpp
auto* gpu_ptr = static_cast<f32*>(staging_buf->map());
auto result = crd::test::ulp_compare(cpu_ref, ConstSpan<f32>{gpu_ptr, N}, /*max_ulp*/ 1);
staging_buf->unmap();
if (!result.ok) {
    INFO("first mismatch at " << result.first_mismatch_index
         << ": cpu=" << result.cpu_value << " gpu=" << result.gpu_value
         << " ulp_diff=" << result.ulp_diff);
}
REQUIRE(result.ok);
```

Caller stages the GPU buffer into a host-visible read-back span first — staging policy depends on whether the buffer is `GpuToCpu` already or needs a copy, so we don't pretend to abstract it away.

### 3. `crd::perf::measure_ms` + `CRD_PERF_BUDGET_LE`

**Header:** `engine/perf/include/crd/perf/measure.hpp`

`measure_ms<F>(F&& fn) -> double` — wall-clock duration in milliseconds via `crd::time::MonotonicClock`. Immune to wall-clock adjustments. Lives in `crd-perf` (not `crd-test-helpers`) so engine code can use the same helper.

`CRD_PERF_BUDGET_LE(name_literal, max_ms, lambda)` — runs the lambda, asserts the wall-clock is ≤ max_ms via `CRD_ASSERT_MSG`. Active in Debug / RelWithDebInfo / ASan / Shipping with asserts on. In pure Release (NDEBUG, asserts off) the macro compiles to a lambda call + `(void)_crd_dur_ms` — the side effects still run, the budget check is silent. For Release-mode enforcement, wrap `measure_ms(...)` in a Catch2 `CHECK` instead.

```cpp
CRD_PERF_BUDGET_LE("lbvh_1m_prims", 8.0, [&]{
    dispatch_lbvh_build();
    fence->wait();
    fence->reset();
});
```

### 4. `crd::test::gpu_determinism_check`

**Header:** `tests/test_helpers/include/crd/test_helpers/gpu_determinism.hpp`

```cpp
template <typename DispatchFn, typename ReadFn>
[[nodiscard]] bool gpu_determinism_check(DispatchFn&&, ReadFn&&, int rounds = 3);
```

Round 0: dispatch + snapshot the bytes via memcpy into an owned buffer. Rounds 1..N-1: dispatch + memcmp against the snapshot. Returns true iff every round produces byte-identical output. Caller obligations:
- `dispatch` must complete + sync (the helper assumes `read_bytes()` returns valid data after dispatch returns).
- `dispatch` must be idempotent w.r.t. inputs.
- `read_bytes()` returns a fresh view per call; the helper snapshots before the next round overwrites.

The discriminating test that separates "deterministic single-pass kernel" (claim must hold) from "throughput-tier atomics ordering varies" (don't claim it).

### 5. `-IncludeRelease` flag on per-slice DoD

**Script:** `scripts/per-slice-check.ps1`

Both parallel branch (line 74) and sequential branch (line 174) now accept `[switch]$IncludeRelease`, which appends `win-release` to the preset list. Opt-in 5th config for slices that need win-release LTCG coverage. The Phase 3.1.7.6 v0-close vtable-stability incident is the precedent — middle-insertion of pure-virtuals in `Device`/`Queue`/`CommandBuffer` shifted vtable slots and caused wrong-method dispatch only in win-release LTCG. The default per-slice DoD remains 4 configs (debug + asan + shipping + tidy) for speed; v9 slices that build GPU kernels should set `-IncludeRelease`.

---

## Mid-slice fixes

### Fix 1: `ValidationCapture::Impl` private nesting blocked the callback

`Impl` was forward-declared as a `private` nested struct, but the free-function `capture_callback` in the `.cpp` needed to `static_cast<ValidationCapture::Impl*>` its `user_data` ptr. C2248: private struct inaccessible. Fix: promoted the forward declaration to `public`. The definition still lives only in the `.cpp`, so internals stay encapsulated; the public name is just an opaque tag.

### Fix 2: `_crd_dur_ms` unreferenced in win-release / win-shipping

`CRD_ASSERT_MSG` compiles to `((void)0)` under NDEBUG, so `const double _crd_dur_ms = ::crd::perf::measure_ms(lambda);` was an unused local in win-release and win-shipping. `/W4 /WX` → C4189 → C2220 build fail. Fix: added `(void)_crd_dur_ms;` immediately after the measure call. Updated the macro comment to be honest about the assert behavior in pure Release (was claiming "works in all build types", which was wrong) and recommended wrapping in Catch2 `CHECK` for Release-mode enforcement.

---

## Per-slice DoD result

`scripts/per-slice-check.ps1 -IncludeRelease -Parallel`:
- win-debug: PASS (build + ctest)
- win-asan: PASS (build + ctest)
- win-shipping: PASS (build + ctest)
- **win-release: PASS (build + ctest)** — first slice run with `-IncludeRelease`
- win-tidy: PASS (build)

Smoke test `crd-test-helpers-tests` (10 cases / 20 assertions): all green. Validates `ulp_compare` (identical / 1-ULP drift at threshold 0 and 1 / +0/-0 equality / NaN rejection), `bit_compare` (identical / first-mismatch index report), `gpu_determinism_check` (byte-stable mock passes / non-deterministic mock fails), and `measure_ms` + `CRD_PERF_BUDGET_LE` (returns positive duration / accepts a budget).

---

## Pattern locked

Every v9 slice from v9c-a onward follows this GPU-sanity discipline at minimum:

1. **Wrap setup in `ValidationCapture`**; assert 0 errors / 0 warnings unless the test is intentionally provoking validation (negative test).
2. **`bit_compare` / `ulp_compare`** GPU readback vs CPU reference for any kernel that has a CPU reference.
3. **`gpu_determinism_check`** for 3 rounds if the slice claims determinism.
4. **`CRD_PERF_BUDGET_LE`** per published per-kernel budget (e.g. v9a-close locks "1M primitives in <8 ms on RTX 3060").

This is the discipline locked at v9-prereq-close. v9c-a (V-HACD voxelize) starts using it tomorrow.

---

## Files touched

- `engine/rhi-vulkan/include/crd/rhi/vulkan_validation_capture.hpp` (NEW)
- `engine/rhi-vulkan/src/vulkan_validation_capture.cpp` (NEW)
- `engine/rhi-vulkan/CMakeLists.txt` (added the new .cpp to the source list)
- `engine/perf/include/crd/perf/measure.hpp` (NEW)
- `tests/test_helpers/CMakeLists.txt` (NEW)
- `tests/test_helpers/include/crd/test_helpers/gpu_compare.hpp` (NEW)
- `tests/test_helpers/include/crd/test_helpers/gpu_determinism.hpp` (NEW)
- `tests/test_helpers/test_helpers_smoke.cpp` (NEW)
- `tests/CMakeLists.txt` (added `add_subdirectory(test_helpers)` at top)
- `scripts/per-slice-check.ps1` (added `[switch]$IncludeRelease` + branch hooks)
- `context.md` (Last shipped milestone updated)
- `docs/phases/phase-3.1.7-geometry.md` (v9-prereq-test-harness row added)

## Next

🎯 **v9c-a V-HACD voxelize** (Mamou 2014 §3.1) — cooker-only, ~4 days, first v9 algorithm slice using the harness from day 1.
