# Session — 2026-07-22 · D-007 table audit (B16 / B-cmp / B4) + the DX12 sort gate

**Ask:** the user flagged that B16, B-cmp, and B4 are not ticked in the detour master table — verify whether they're actually
full, finish any genuine gaps, then move on (in order) to C6 and report.

## Audit findings

**B4 (mesh-shading pipeline) — was 🚧, now ✅ (stale marker).** Re-ran every B4 gate on both backends: **Vulkan 128 asserts /
13 cases**, **DX12 86 / 11** — mesh core · task/amplification · multi-field payload · GPU-driven compute-cull→indirect dispatch ·
per-primitive VRS · tessellation · B4-vis (software rasterizer, DAIS deferred shade, HZB occlusion cull, HW-raster VBuffer) — all
green. The "remaining" items listed in the row (payload>1uint, compute-cull→dispatch, VRS) were all subsequently landed; the only
open piece — the WGSL `texture_2d_array` bindless-LOD cascade — is explicitly scoped to the WebGPU deployment milestone (WGSL is
compute-only here by design), not D-007, and the mesh↔cluster-AS bridge is done in C3/B9 (`build_scene_clusters`). → flipped to ✅.

**B-cmp (hesap-GPU compute primitives) — was ⬜, now ✅ (one real gap, closed).** Verified each primitive is bit-exact on the CPU
oracle + both production backends:
| primitive | kir oracle | Vulkan | DX12 |
|---|---|---|---|
| FFT (radix-2/4/16, batched 2-D, fused conv) | ✓ | ✓ | ✓ |
| scan / stream-compaction | ✓ | ✓ | ✓ |
| reduction | ✓ | ✓ | ✓ |
| small GEMM / MLP + backprop | ✓ | ✓ | ✓ |
| **GPU radix sort** | ✓ | ✓ | **✗ → added** |

The GPU sort was validated on Vulkan + oracle but had **no DX12 gate**. Added `[dx12][compute][gpu][kernel][sort]` — the 4-pass
radix sort (histogram → parallel-offset → gbase → scatter, 12 dispatches, ping-pong buffers via the shared `IComputeContext`
record API) dispatches on DX12 and produces a fully-sorted permutation (22 asserts; self-verifying: monotone readback + XOR
permutation check). → flipped to ✅.

**B16 (ocean) — ◧.** DoD + the cinematic visual pass are closed (previous session). The remaining scope (screen-space refraction,
underwater Beer/god-rays, caustics, planar/SSR-for-water, wave-particle↔FFT coupling) is genuinely deferred and needs underwater /
scene content to be meaningful — a real slice, left for a dedicated pass (not part of this audit). B12 already owns generic Hi-Z
SSR (`ssr_reflect`/`ssr_hiz_hit`/`ssr_edge_fade`/`ssr_confidence`), which the water can reuse when that slice runs.

## The real bug the sort gate caught — an HLSL compute-emitter gap

Adding the DX12 sort gate immediately surfaced a genuine portability gap: `emit_compute_kernel_hlsl` returned **false** on the
scatter kernel. Root cause: the subgroup (wave) ops `SubgroupBallot` / `SubgroupBallotExclCount` / `SubgroupMatch` were wired for
GLSL (`subgroupBallot(.).x` etc.) but **not HLSL** — the histogram/offset/gbase kernels don't use them, so nothing had exercised
the HLSL path before. This is the recurring "COMPUTE emitter lagged an op ⇒ emit FALSE; wire ALL backends" scar. Fixed in
`ckir_hlsl.hpp` (the SM6.0 wave intrinsics):
- `SubgroupBallot(pred)` → `WaveActiveBallot(pred != 0u).x` (uint4 → `.x` under the forced 32-lane subgroup)
- `SubgroupBallotExclCount(mask)` → `countbits(mask & ((1u << WaveGetLaneIndex()) - 1u))` (exclusive rank of an arbitrary mask)
- `SubgroupMatch(v)` → `WaveMatch(v).x` (SM6.5; only the onesweep-hw_match path uses it — the 4-pass sort needs only the first two,
  which are SM6.0, and the DX12 compute context compiles `cs_6_0`)

## Verification

- DX12 `[sort]` — 22/1 GREEN (new gate).
- DX12 B4 gates — 86/11; Vulkan B4 gates — 128/13.
- kir `[sort],[kernel]` — 201/48 (no emit regression from the `ckir_hlsl.hpp` change).
- clang-tidy (LLVM-20.1.8) on `ckir_hlsl.hpp` + `test_dx12_compute.cpp` — clean.

## Files

- `engine/kir/include/crd/kir/ckir_hlsl.hpp` — 3 subgroup wave-intrinsic cases in the compute value emitter.
- `tests/gpu-context-dx12/test_dx12_compute.cpp` — `+#include <crd/kir/ckir_sort.hpp>`; the DX12 radix-sort gate.
- `docs/detours/D-007-gpu-program-system.md` — B4 🚧→✅, B-cmp ⬜→✅ with audit notes.

## Proposed commit (user commits — no AI co-author trailer)

```
test(b-cmp): DX12 radix-sort gate + wire subgroup wave-ops into the HLSL compute emitter

Auditing the D-007 table: B4 (mesh) and B-cmp (compute primitives) were unticked
but effectively complete. B4 is a stale marker (all gates green both backends).
B-cmp had one real gap — the GPU sort was validated on Vulkan + oracle but never
on DX12.

Adding the DX12 sort gate surfaced an HLSL compute-emitter gap: SubgroupBallot /
SubgroupBallotExclCount / SubgroupMatch were wired for GLSL but not HLSL, so the
radix-sort scatter kernel failed to emit. Wire them to the SM6.0 wave intrinsics
(WaveActiveBallot / countbits+WaveGetLaneIndex / WaveMatch). The 4-pass radix sort
now dispatches on DX12 == a fully-sorted permutation (22 asserts).

DX12 [sort] 22/1, kir [sort],[kernel] 201/48, DX12/Vulkan B4 gates green; tidy-clean.
```
