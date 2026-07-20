# Session — 2026-07-20 — B18-f: the RT strand tier (linear swept spheres)

## Goal

Close B18-f — hair strands as a **ray-traced** primitive — and with it the last open items of C3 and B9.
The requirement was a first-class portable strand tier, not a fallback: analytic swept-sphere intersection
authored in CKIR, running on both backends, shading with the same B18-a BCSDF the raster tier uses.

## What we built / changed

- **`engine/kir/include/crd/kir/ckir_lss.hpp`** (new) — the swept-sphere primitive.
  `lss_intersect` (analytic ray / round cone), `build_lss_aabb_kernel` (conservative per-segment bounds),
  `build_lss_trace_kernel` (host-unrolled reference sweep), and `build_rt_hair_shade_kernel` — the tier's
  payoff: one `TraceRayCurves` per pixel, fibre frame rebuilt from the hit segment, shaded with the
  Chiang BCSDF.
- **`KStmtKind::TraceRayCurves`** — a new CKIR statement: trace against a procedural curve BLAS, returning
  `RtCurveHit{t, u, prim}`. Oracle in `ckir_kernel_eval.hpp` (brute force, in **float** per the oracle
  doctrine), plus GLSL and HLSL emission of the candidate-intersection loop.
- **`build_scene_curves`** on both `VulkanRayTracingContext` and `Dx12RayTracingContext` — one procedural
  AABB per segment, non-opaque BLAS, single-instance TLAS.
- **Vulkan LSS extension detection** (`VK_NV_ray_tracing_linear_swept_spheres`) + `RtFeature::LinearSweptSpheres`
  in the unified capability query. Verified absent on this adapter with a positive control.
- **`eval_cpu_kernel` now REFUSES vector-valued nodes** instead of silently evaluating them to garbage.

## Plain-English explanation

A hair strand is a chain of segments, and the natural ray-tracing shape for a segment is a sphere dragged
along it — a capsule, or a cone with rounded ends when the strand tapers. NVIDIA put that shape in silicon
on Blackwell; this machine is Ada, and DirectX has no equivalent at any tier. So the portable path is not a
consolation prize, it is the path: the acceleration structure holds one box per segment, and our own shader
works out where the ray meets the swept sphere.

The reason that is worth doing rather than just chopping each strand into triangles: a 170,000-strand groom
at 30 segments is 5 million segments. Tessellated, that is 40+ million triangles — gigabytes of structure,
and the silhouette is *still* faceted when the camera gets close. One box per segment is ~24 bytes and the
curve stays exactly round at any zoom.

The last piece is shading. A hit gives you a distance and a position along the strand, but the hair model
needs the strand's *direction* — so the hit record also carries which segment won, and the shader rebuilds
the fibre frame from it.

## Decisions made

- **The hit record carries `prim`.** Distance and axial coordinate are not sufficient to shade: the BCSDF is
  defined in the fibre frame and the tangent is a property of the segment. Read back from the *committed*
  intersection rather than tracked through the candidate loop, so it cannot drift out of step with `t`.
- **The strand tier shades with the B18-a BCSDF, not a stand-in.** A raster tier and an RT tier that disagree
  about what a fibre looks like would be two materials wearing one name.
- **Compute-tier vector maths is written component-wise on scalar nodes.** The `vec3` forms belong to the
  raster tier, where the emitters lower them to native vector types. This is now enforced, not conventional.

## Debugging scars (the expensive ones)

- **⛔⛔ The round-cone intersector had FOUR independent defects**, every one of them invisible for a constant-radius
  capsule (`rr == 0`) and therefore invisible to the first four gates: `d2 = m0 + rr²` instead of `m0 − rr²`;
  the k-coefficients scaled by `m0`; the axial span tested against `m0` instead of `d2`; and the ray direction
  **not normalised** (the reference form assumes a unit direction). Together they reported hits in empty space —
  **118 of 132 reported hits were off-surface** against a dense ray-march ground truth. Found by building that
  ground truth in Python and comparing, not by reading the code again. Fixed in all four places (IR, oracle,
  GLSL, HLSL); corrected Python then reported 57 hits, zero off-surface, matching the GPU's 57 exactly.
- **⛔ `rayQueryGenerateIntersectionEXT` needs a `tHit` inside the ray's CURRENT range**, which traversal narrows
  on every commit — seeding the candidate search from the ray's original `tmax` clobbers nearer hits. Symptom:
  a committed `t` of 7.7e-05 where the answer was 0.95.
- **⛔⛔ `eval_cpu_kernel` is SCALAR and had no case for `Vec3`/`VecComp`/`Swizzle`.** They fell through to
  `apply_ternary`/`apply_unary`, which do not implement them, and evaluated to garbage **with no diagnostic**.
  This cost an hour on B18-f's shading gate: `h` was provably correct and the frame vectors were provably
  correct, but the `vec3` carrying them into the BCSDF collapsed its z component to 0, so φ came out 0 instead
  of π/2 — and the result was *symmetric in h*, which is physically plausible and completely wrong. The
  evaluator now asserts rather than guessing.
- **⛔ Re-materialising an already-materialised node allocates a SECOND slot that nothing writes.**
  `TraceRayCurves` materialises all three results itself; calling `stmt_materialize` on them again made `t`
  read back as 0 on every lane, which defeated the miss mask and made every ray look like a near hit.
- The taper gate's expectation was itself an approximation. The exact answer is `t = dist − r(u)/cos α` with
  `sin α = (ra − rb)/L`; it holds to 1e-5, so the tolerance was tightened 2e-4 → 2e-5 rather than left loose.

## Files touched

- `engine/kir/include/crd/kir/ckir_lss.hpp` — new: the primitive, its AABBs, the reference sweep, the shading kernel
- `engine/kir/include/crd/kir/ckir.hpp` — `TraceRayCurves` + `RtCurveHit{t,u,prim}`; `KBuiltin::HitBary` in `builtin_info()`
- `engine/kir/include/crd/kir/ckir_kernel_eval.hpp` — `TraceRayCurves` oracle; vector-node refusal; RT-statement cases
- `engine/kir/include/crd/kir/ckir_glsl.hpp`, `ckir_hlsl.hpp` — candidate-loop emission for both dialects
- `engine/gpu-context/include/crd/gpu/rt_capabilities.hpp` — `RtFeature::LinearSweptSpheres`
- `engine/gpu-context-vulkan/src/vulkan_ray_tracing_context.cpp` — `build_scene_curves`; `build_as` hoisted out of two byte-identical lambdas
- `engine/gpu-context-vulkan/src/vulkan_context.cpp` / `.hpp` — LSS extension detection + feature chain
- `engine/gpu-context-dx12/src/dx12_ray_tracing_context.cpp` / header — `build_scene_curves` (procedural-AABB BLAS)
- `tests/kir/test_ckir_lss.cpp` — 8 gates incl. the RT shading gate
- `tests/kir/test_ckir_curve_rt.cpp` — oracle vs closed form; oracle vs the IR trace kernel
- `tests/gpu-context-vulkan/test_vulkan_rt.cpp`, `tests/gpu-context-dx12/test_dx12_rt.cpp` — hardware gates

## Tests / verification

- Built ✅ (win-debug, all three suites)
- `[lss]` CPU gates: **8/8**, 6740 assertions
- Vulkan hardware curve traversal: 256 rays over 36 segments, 45 hits, **zero** hit/miss disagreements vs the
  oracle, maxabs 8.3e-05
- DX12 hardware curve traversal: same scene, same seed, same oracle — **7/7**
- RT shading gate: the IR fibre frame checked against a **second, plain-C++ frame construction** sharing no code
  with it, at 8 azimuthal offsets, agreeing to < 2e-5

## Next session starts with

- clang-tidy on the new files (`ckir_lss.hpp`, `test_ckir_lss.cpp`, `test_ckir_curve_rt.cpp`) — per-slice, per file
- the full `scripts/per-slice-check.ps1` sweep, then B18 close-out (bench board + phase row)
