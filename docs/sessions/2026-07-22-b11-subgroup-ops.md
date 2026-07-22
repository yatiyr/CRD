# Session — 2026-07-22 · B11 wave/subgroup op class in CKIR (bit-exact both backends)

**Ask:** carry on the D-007 main line, no gaps, a performant frontier system with CKIR. Next in order after B10: **B11** (wave/
subgroup + quad ops + work-graph node shaders). Hardware check: the full subgroup suite is supported; work graphs
(`VK_AMDX_shader_enqueue`) are NOT on this NVIDIA device → the work-graph halves of B11/C5 are env-blocked, subgroup ops are fully
buildable and CKIR-central.

## What landed

The full **wave / subgroup op class** in CKIR (appended at END of `KOp` for cook stability, joining the existing
`SubgroupBallot/ExclCount/Match`):
- **reduce:** `SubgroupAdd`, `SubgroupMin`, `SubgroupMax`, `SubgroupAnd`, `SubgroupOr`, `SubgroupXor`
- **prefix scan:** `SubgroupInclusiveAdd`, `SubgroupExclusiveAdd`
- **data movement:** `SubgroupBroadcastFirst`, `SubgroupShuffle`

Builders on `KGraph` (`subgroup_add`/`_min`/…/`_shuffle`); the result carries the operand type.

**Emit** — wired on both production backends, in every emit path (no "emit false" gaps):
- GLSL: `subgroupAdd`/`subgroupMin`/…/`subgroupInclusiveAdd`/`subgroupExclusiveAdd`/`subgroupBroadcastFirst`/`subgroupShuffle`,
  plus the `GL_KHR_shader_subgroup_arithmetic` + `_shuffle` extensions; added to the fused-elementwise path, the `self` kernel
  path, and the `pv` kernel path.
- HLSL (SM6.0): `WaveActiveSum`/`Min`/`Max`/`BitAnd`/`BitOr`/`BitXor`; `WavePrefixSum` is EXCLUSIVE so inclusive =
  `WavePrefixSum(x)+x`; `WaveReadLaneFirst`/`WaveReadLaneAt`; added to the pv kernel path AND the fused elementwise path;
  registered in the shared `is_fusable`.
- CPU oracle: 32-lane subgroup simulation for every op. INTEGER reductions/scans are order-independent ⇒ **bit-exact**; sums
  accumulate in f64 (32·u32 < 2^53) then `round_dtype` wraps mod-2^32 (modular add is associative ⇒ == the GPU's per-step wrap);
  bitwise stay in u32; min/max compare; broadcast/shuffle index the source lane.

## Verification

`[subgroup]` gate (shared kernel `tests/gpu-shared/ckir_subgroup_test.hpp` — one 64-thread workgroup = two 32-lane subgroups, 7
results/thread): the GPU dispatch matches the CPU oracle **BIT-EXACT** on **Vulkan (0/448 mismatches)** and **DX12 (== Vulkan)**.
The two backends agree with each other and with the oracle. clang-tidy (LLVM-20.1.8) clean on all changed headers + tests. Full kir
regression re-run (the `KOp`/emit/oracle changes touch every kir test).

## Why it matters (performance)

Subgroup ops are THE fast primitive for cross-lane work: a workgroup reduction or scan done with a few `subgroupShuffle`/
`subgroupInclusiveAdd` is far cheaper than a shared-memory log-tree with `barrier()`s at each step (no shared memory, no barriers,
one instruction per step on the hardware). They're the fast path for workgroup reductions, stream compaction, histogram binning,
and the radix-sort rank (which already uses `SubgroupBallot`). Making them a first-class, **bit-exact/portable** CKIR op class is
the mission bar — most engines' subgroup reductions are non-deterministic across drivers; integer CKIR reductions are not.

## Remaining B11 (explicit)

- **quad ops** (SM6.6 — `subgroupQuad*` / `QuadReadAcross*`, 4-lane groups) — buildable, a follow-on sub-slice.
- **work-graph NODE shaders** (`SPV_AMDX_shader_enqueue`) — **env-blocked**: NVIDIA Vulkan does not expose
  `VK_AMDX_shader_enqueue`, and D3D12 Work Graphs need the Agility SDK (not vendored) — documented like the coopvec-DX12 block.
- device-generated-command shaders (pairs with C5).

## Proposed commit (user commits — no AI co-author trailer)

```
feat(b11): CKIR wave/subgroup op class — reduce/scan/broadcast/shuffle, bit-exact both backends

Append SubgroupAdd/Min/Max/And/Or/Xor, SubgroupInclusive/ExclusiveAdd,
SubgroupBroadcastFirst, SubgroupShuffle to KOp (cook-stable). Wire GLSL
(subgroup* + arithmetic/shuffle extensions) + HLSL (WaveActive*/WavePrefixSum/
WaveReadLane*) in every emit path; extend the CPU oracle (32-lane sim, integer
reductions bit-exact).

[subgroup] gate: a 64-thread workgroup (2x32-lane) exercising the ops dispatches
== the CPU oracle bit-exact on Vulkan (0/448) and DX12 (== Vulkan). Shared kernel
tests/gpu-shared/ckir_subgroup_test.hpp.
```
