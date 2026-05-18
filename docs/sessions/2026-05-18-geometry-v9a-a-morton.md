# 2026-05-18 — Phase 3.1.7 v9a-a GPU Morton-code generation ✅ SHIPPED

**Slice:** Phase 3.1.7 v9a-a — first slice of the v9a `-gpu` LBVH cluster + first slice in a brand-new sibling module `crd-geometry-bvh-gpu`. Opens GPU geometry pipeline for Cerid.

**Status:** ✅ shipped same day. **5-config DoD PASS** via `scripts/per-slice-check.ps1 -IncludeRelease -Parallel`.

---

## What landed

### New sibling module `crd-geometry-bvh-gpu` (D132)

- `engine/geometry-bvh-gpu/` — mirrors the `crd-rhi` / `crd-rhi-vulkan` split. Existing `crd-geometry-bvh` stays CPU-only (no Vulkan dependency); GPU kernels + dispatch live here.
- Deps: `crd-geometry-bvh` (CPU BvhTree types for v9a-c output), `crd-rhi` (backend-agnostic surface), `crd-rhi-vulkan` (impl-only).
- Future GPU geometry kernels (GPU radix sort, LBVH tree, AABB upsweep, BVH refit) land here too.

### 30-bit Morton-code generation

CPU oracle in `morton.hpp`:
- `spread_bits_30(u32)` — Lauterbach 2009 bit-interleave. Insert 2 zero bits between each of the low 10 bits.
- `morton3_30bit_from_ints(ix, iy, iz)` — combine three 10-bit indices into a 30-bit Morton code with pattern `z9 y9 x9 ... z0 y0 x0`.
- `quantize_to_morton_grid(v, lo, inv_extent)` — world coord → `[0, 1023]` bin via truncate-toward-zero + clamp.
- `compute_morton_codes_cpu(span<AABB>, alloc) -> Array<u32>` — batch driver.

GPU pipeline in `dispatch.hpp`:
- `MortonGpuPipeline` — cached compute pipeline + descriptor allocator + per-call dispatch. Ctor loads SPIR-V + creates pipeline objects once; `dispatch_morton_codes` is upload + dispatch + readback per call.

GLSL shader in `runtime/examples/shaders/compute_morton_codes.comp`:
- **Mechanical translation of the CPU kernel body.** Same bit pattern, same FP quantisation, same scene-AABB normalisation. The CPU sequential IS the algorithm definition; the GPU shader is a verbatim copy of the per-element loop body. Any divergence is a bug.

### Scope honesty: 30-bit lock (D133)

30-bit Morton = 1024³ resolution per axis. Practical:

| Scene extent | Resolution |
|---|---|
| 100 m | ~10 cm |
| 1 km | ~1 m |
| Orbital scale | ~km |

Locked at v9a-a for game / sim / CAD scenes at ≤ 100 m extent. `v9a-60bit` follow-on filed for eylem-aero and CAM at km-scale.

---

## Test corpus (advisor TDD — CALIBRATION FIRST)

9 cases / 78 assertions in `tests/geometry-bvh-gpu/test_morton.cpp`:

1. **CALIBRATION FIRST**: `spread_bits_30` lane-pinned bit patterns (7 known input → output pairs across all 10 lanes).
2. **CALIBRATION**: `morton3_30bit_from_ints` canonical triples (origin → 0, axis-aligned units → bit positions, max → 0x3FFFFFFF).
3. **CALIBRATION**: `quantize_to_morton_grid` endpoints + clamping (0 / 0.5 / 1.0 / out-of-bounds / zero-extent).
4. CPU empty input → empty output.
5. CPU corner-AABB test: 8 corner centroids → 8 distinct Morton codes, with `codes[0]==0` and `codes[7]==morton(1022,1022,1022)` exactly.
6. `union_aabb_of` empty + single-span.
7. **GPU bit-compare**: 1024 random-ish AABBs in unit cube → CPU oracle vs GPU dispatch must be **byte-identical**.
8. **GPU determinism**: `gpu_determinism_check(3 rounds)` on 256 AABBs.
9. **GPU perf budget**: 256k AABBs end-to-end (staging + dispatch + readback) under 200 ms.

All ValidationCapture assertions: `error_count() == 0`, `warning_count() == 0`.

---

## Mid-slice bugs (solved honestly, none deferred)

### Bug 1 — `centroid` is a reserved GLSL keyword

GLSL parser rejected the local variable `centroid` (it's a GLSL interpolation qualifier). Renamed to `mid` in the shader. Mechanical fix; no impact on the algorithm.

### Bug 2 — Corner Morton test math was wrong

Original test placed AABB centroids at `(eps, eps, eps)` with `eps=0.001` expecting Morton=0. But `quantize(0.001, 0, 1.0) = floor(0.001 * 1024) = 1`, so Morton = `morton3(1,1,1) = 7`. Fix: use exact `(0.0, 0.0, 0.0)` for the low corner and `(0.999, 0.999, 0.999)` for the high corner so quantise lands on bins (0,0,0) and (1022,1022,1022) deterministically.

### Bug 3 — Cross-queue-family submit (mine)

I submitted via `device.compute_queue()` but `create_command_buffer()` allocates from the graphics-family command pool. On GPUs with a dedicated compute family (RTX 4070 Ti SUPER here), this trips `VUID-vkQueueSubmit-pCommandBuffers-00074`. **Fix**: submit through `graphics_queue()` — works for both single-queue and split-queue GPUs. True async-compute (compute-family command pool + submit) is the `v9a-a-async-compute` follow-on; needs the RHI to surface a compute-family command pool.

### Bug 4 — Pre-existing rhi-vulkan bug surfaced

v9a-a is the **FIRST consumer** to attach a `ValidationCapture` to a real `VkInstance`. It surfaced a longstanding rhi-vulkan bug: device creation unconditionally enables `VK_KHR_swapchain` without the instance enabling `VK_KHR_surface` first (VUID-vkCreateDevice-ppEnabledExtensionNames-01387). When GLFW is initialised, `glfwGetRequiredInstanceExtensions` adds `VK_KHR_surface`; without GLFW (typical for compute-only tests), nothing did. **Fixed in `engine/rhi-vulkan/src/vulkan_backend.cpp` as part of this slice** by unconditionally adding `VK_KHR_surface` to enabled instance extensions when available. Per [[never-defer-solve]]: solve, don't defer.

### Bug 5 — Tidy cleanup

`_pad_a` / `_pad_b` / `_pad_c` member names tripped `readability-identifier-naming` (underscore-prefixed not allowed for public members). Renamed to `pad_a` / `pad_b` / `pad_c`. `morton3_30bit_for_centroid` using-decl unused in test → removed. Nested `for` chain without braces tripped `readability-misleading-indentation` → added braces.

---

## Pattern locked per `feedback_v9_gpu_sanity_harness`

This is the first GPU slice that fully exercises the v9-prereq-test-harness:
- **ValidationCapture** wraps every dispatch test → assert `error_count() == 0` + `warning_count() == 0`. Surfaced a real engine bug (Bug 4). The harness is doing exactly what it was built to do.
- **`crd::test::bit_compare<u32>`** for byte-identical GPU vs CPU oracle. 1024 codes, all match.
- **`crd::test::gpu_determinism_check(3 rounds)`** for the pure-function kernel. Passes.
- **`CRD_PERF_BUDGET_LE`** at 200 ms end-to-end for 256k AABBs. Wall-clock includes staging + readback; pure-GPU timing isolation is the v9a-close timestamp-query slice.
- **`-IncludeRelease` 5-config DoD** — every GPU slice exercises fresh kernel + validation messenger code = LTCG risk surface, run all 5 configs every slice.

---

## Pinned design decisions (carried for ADR-0076 §25 amendment at v9a-close)

- **D132** — Module structure: NEW sibling `crd-geometry-bvh-gpu`. Mirror of `crd-rhi` / `crd-rhi-vulkan` split.
- **D133** — 30-bit Morton default. 60-bit u64 path = `v9a-60bit` follow-on.
- **D134** — CPU reference IS the algorithm definition. GPU GLSL is a mechanical translation. `bit_compare` enforces.
- **D135** — Sync-compute dispatch via `graphics_queue()` (matches current command-pool family). True async-compute = `v9a-a-async-compute` follow-on.

---

## Files touched

**New module** `engine/geometry-bvh-gpu/`:
- `CMakeLists.txt`
- `include/crd/geometry/bvh_gpu/morton.hpp` — CPU oracle + bit-interleave primitives.
- `include/crd/geometry/bvh_gpu/dispatch.hpp` — `MortonGpuPipeline`.
- `src/morton.cpp` — CPU oracle impl.
- `src/dispatch.cpp` — GPU pipeline impl.

**New tests** `tests/geometry-bvh-gpu/`:
- `CMakeLists.txt`
- `test_morton.cpp` — 9 cases / 78 assertions.

**New shader** `runtime/examples/shaders/compute_morton_codes.comp` — mechanical GLSL translation of CPU kernel.

**Engine fix** `engine/rhi-vulkan/src/vulkan_backend.cpp` — surfaced + fixed VK_KHR_surface omission (Bug 4).

**Wired** `CMakeLists.txt` + `tests/CMakeLists.txt` to add the new module + test dirs.

**Docs** `context.md` + `docs/ROADMAP.md` + `docs/phases/phase-3.1.7-geometry.md` + new `docs/systems/geometry-bvh-gpu.md`.

## Next

🎯 **v9a-b1 CPU radix sort** — deterministic `(morton_code, original_index)` pair sort via `crd::containers::sort`. Throughput-tier (~1 ms / 1M elements on CPU). THE CORRECTNESS REFERENCE for v9a-b2 GPU radix sort. ~3 days, ~300 LOC engine + ~200 tests.
