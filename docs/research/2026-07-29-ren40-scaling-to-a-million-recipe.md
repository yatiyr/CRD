# REN-40 — scaling to a million: what the 2026 frontier actually does, and what Cerid should build

> **Outcome:** **adopted** — REN-40 shipped (device-side cull, incremental extract, GPU skinning, frame tricks, cluster-DAG LOD; sessions 2026-07-30…08-02). *(stamped 2026-08-07, doc-hygiene pass)*

**Study → recipe.** This is the survey that precedes 40-A, written against the measured start line in
`docs/bench/2026-07-29-ren40-million-instance-baseline.md` (1M instances = **337 ms CPU / 90 ms GPU, ~3 fps**,
CPU-bound: extract 171 ms, CPU cull ~130 ms, uploads 32 ms, 256k instances drawn at full detail).

Every section ends with **→ CERID**: what we take, what we skip, and *why*, given what this engine already has.

> ⛔⛔⛔ **THE STANDING RULE THIS DOCUMENT IS WRITTEN UNDER** (user, 2026-07-29): *use every ability of every API
> and incorporate it into the general gpu-context; where a backend lacks one, fall back to that backend's own
> equivalent — but always do the best it can.* A technique is never rejected because "the other API can't", and
> a mechanism is never chosen because it is the intersection of both. The shared verb declares the **intent**;
> each backend implements it with the **best mechanism it has**; any step-down is **named and reported**.
> Identical code is not the goal — identical *results* at each backend's top speed is.

---

## 0. What Cerid already owns (so the recipe builds on it, not beside it)

| Substrate | Where | Status |
|---|---|---|
| Indirect multi-draw, both backends | `draw_storage_multi_indexed_depth(_only)` → `vkCmdDrawIndexedIndirect` / `ExecuteIndirect` | live, REN-39-A2/C1 |
| A **DrawIndex** channel that works on both APIs | `gl_DrawIDARB` (GLSL 450) / root-constant b7 varied per command (DX12) | live, REN-38 |
| A per-draw **draw table** (row → that draw's buffer base) | `kSceneDrawTableOff`, the rebased program | live, REN-38 |
| GPU cull chain with real indirect args + instance-count range guard | 38-F15, header word 100 | live but **only reachable from an authored graph**, not the default path |
| Mesh + **amplification** shaders that PULL REAL GEOMETRY | B4 + F16 (`[mesh] fetch = true`, `[task] emit_header = N`) | live, both backends |
| Visibility-buffer helpers | `ckir_visbuffer.hpp` | live |
| Bindless textures (1024) | REN-38 | live |
| Compute in the same IR as raster | CKIR | live |

⭐ The conclusion that shapes everything below: **Cerid does not need new API plumbing for GPU-driven rendering.
It needs the cull moved onto the device and a LOD concept.** The verbs, the DrawIndex channel and the draw table
are already there and already portable.

---

## 1. GPU-driven culling — the core loop

The canonical shape (vkguide, AnKi, and every GPU-driven talk since Assassin's Creed 2015):

1. Instance data lives **persistently on the GPU**. The CPU never walks it per frame.
2. A compute pass reads per-instance bounds + the frustum(s) and **compacts survivors**, writing both the
   compacted instance-index list and the indirect draw args.
3. The draw consumes those args with multi-draw-indirect. Zero CPU per-instance work per frame.

**Compaction: atomics vs prefix sum.** vkguide's version does one `atomicAdd(draw[batch].instanceCount, 1)` per
surviving instance and writes to `firstInstance + countIndex`. Simple, but at 1M survivors that is up to 1M
atomics on one cache line per batch. Interplay of Light's refinement is the one to take: **scalarize with wave
intrinsics** — `WavePrefixSum` over the wave's survivor mask, ONE atomic per wave for the base, then each lane
writes at `base + its prefix`. ~64× fewer atomics at 1M scale. Cerid already has the primitives
(`subgroup_ballot`, `subgroup_ballot_excl_count`, B11) and they are **bit-exact integer ops**, so this stays
inside the determinism mandate.

**The COUNT buffer — use it, do not dodge it.** ⛔⛔⛔ **STANDING RULE (user, 2026-07-29): use every ability of
every API, incorporated into the general gpu-context; where a backend lacks one, fall back to ITS equivalent —
but always do the best that backend can. Never pick the lowest common denominator "for portability".**
An earlier draft of this recipe chose vkguide's zero-count trick (pre-allocate one slot per batch, let invisible
batches fall to `instanceCount = 0`) *precisely because* it needs no feature bit on either API. That is LCD
reasoning dressed as portability, and it is wrong here: **both backends already have the real thing** —
D3D12's `ExecuteIndirect` takes a `pCountBuffer`, and Vulkan has `vkCmdDrawIndirectCount` (core 1.2 /
`VK_KHR_draw_indirect_count`). A GPU-written count means the device decides how many commands to execute, so
empty batches cost *nothing* rather than a zero-instance command each — which is the whole point at 1M.

→ **CERID:** the count-buffer path is the **PRIMARY on both backends**, queried as a device capability at init.
The zero-count form survives only as the **named, reported fallback** for a device that reports the feature
missing (the same `requires`/`fallback` step-down doctrine the frame assets already use — a step-down nobody can
observe is how "portable" quietly becomes "slow everywhere"). The verb declares the INTENT ("execute up to N
indirect commands, count from this buffer at this offset"); each backend implements it with the best mechanism
it has. Identical *code* is not the goal; identical *results* are.

**⛔ THE CERID-SPECIFIC TRAP.** The canonical recipe writes survivors at `firstInstance + countIndex` and reads
them back via `gl_InstanceIndex`. **Cerid cannot do that**, and `IRasterContext::IndexedDraw` already says why:
Vulkan folds `firstInstance` into `gl_InstanceIndex` and D3D12's `SV_InstanceID` does **not** — a verb that let
a caller set it would let the two backends read DIFFERENT instances. The three arg fields are deliberately the
only ones exposed.

→ **CERID:** compact into a **per-batch region at a fixed base**, and have the VS find its base from
**DrawIndex → the draw table** (both already portable here), exactly as the REN-38 rebased program already does
for the scene buffer. `first_instance` stays 0 forever. The cull kernel writes
`visible[batch_base + local_index] = instance_id`; the VS reads `batch_base` from the table row DrawIndex hands
it. This is strictly *more* portable than the reference recipe and costs nothing.

**⛔ THE COMMAND LAYOUT IS *NOT* IDENTICAL — a correction to this document's own first draft.** The reference
material is right that `D3D12_DRAW_INDEXED_ARGUMENTS` and `VkDrawIndexedIndirectCommand` are binary-identical
(index_count, instance_count, first_index, base_vertex, first_instance), and I concluded from it that one
GPU-written buffer feeds both backends unchanged. **It does not.** Cerid's D3D12 command signature PREPENDS a
root constant carrying DrawIndex — and D3D12 requires the draw argument to be **last** in a signature — so:

| backend | command layout | stride |
|---|---|---|
| Vulkan | `[5×u32 args]` (DrawIndex arrives as `gl_DrawIDARB`) | **20 B**, args at offset 0 |
| D3D12 | `[u32 draw_index][5×u32 args]` | **24 B**, args at offset **4** |

Same fields, different ORDER and STRIDE. Levelling Vulkan up to 24 B does not help either — the *order*
differs, and dropping DX12's root constant would cost it the DrawIndex channel that a rebased program uses to
find its region.

→ **CERID:** the layout is DECLARED, not assumed — `indirect_command_stride()` / `indirect_command_arg_offset()`
on `IRasterContext`, and the GPU producer writes each backend's own form at
`base + i·stride + arg_offset` (writing the leading u32 as DrawIndex where `arg_offset != 0`). That is the
standing rule applied to DATA LAYOUT rather than to verbs: adapt to what each API wants, never level either one
down. ⭐ This is exactly the class of thing that would otherwise have shipped as "works on Vulkan, garbage on
DX12" — the same shape as the clip-space-Y scar.

---

## 2. Occlusion culling — two-pass HZB

The 2026 default (UE5, Bevy 0.16, AnKi): **two-phase**.

- **Phase 1** draws the set that was visible *last* frame (an excellent occluder guess), then builds a
  hierarchical depth pyramid (HZB) from the resulting depth.
- **Phase 2** tests *everything* against that HZB and draws whatever became newly visible.

This converges in one frame, needs no CPU occluder selection, and reuses the same cull kernel with a different
input set. Pitfall from vkguide: a pipeline barrier is required between each HZB mip generation step and before
the cull reads it.

→ **CERID:** the right shape, but **not in 40-A**. Our 1M measurement culls 1M → 256k on frustum alone, and the
remaining cost is CPU-side, not overdraw. Occlusion belongs after 40-A/40-C, when the frustum survivors are the
bottleneck. Recorded here so the cull kernel is *designed* to accept a second (HZB) test rather than retrofitted.

---

## 3. LOD — the elegant answer is cluster LOD, not a discrete chain

Nanite's model, which is now the reference everyone measures against:

- Meshes split into **clusters of ≤128 triangles**; clusters grouped, simplified, and re-split to build a **DAG**
  (not a tree — that is what avoids cracks at group boundaries).
- Each cluster carries a **screen-space error**; the DAG is built so a parent's error is always **larger** than
  any child's. That monotonicity is what makes selection a *local* decision.
- **Selection rule:** render a cluster iff `parent_error > 1 px` and `own_error ≤ 1 px`. No global LOD state, no
  popping, and it parallelises perfectly.
- Runtime is a **three-level GPU cull**: instance → BVH node → cluster, with the HZB tested and the error
  evaluated at each node.
- Geometry is paged and streamed like virtual texture pages, decompressed on the GPU.

→ **CERID:** this is the destination for 40-C, and it lands on the **F16 amplification path**, which already
pulls real geometry and can emit its own meshlet count — that is precisely a cluster-culling task shader. Two
honest caveats: (a) the DAG *build* is an offline cooker feature (cluster → group → simplify → re-split), which
is a `tools/asset_cooker` slice, not a renderer one; (b) full virtualized streaming is out of scope for REN-40.
**40-C should ship the selection rule and the cluster cull over a cooked cluster DAG, and skip streaming.**
⛔ And the shadow-specific rule this engine already learned the hard way: a cascade-3 caster sits at 0.18
world-units/texel, so **shadow passes must select a coarser LOD than the forward pass** — same DAG, different
error threshold, because the error is in *pixels of the target being rendered*.

---

## 4. Shadows — Virtual Shadow Maps are the frontier, and page CACHING is the actual win

VSM (UE5 default, replacing CSM):

- One **16k × 16k virtual** shadow map per light, split into **128×128 pages**.
- Pages are allocated **only where on-screen pixels need them**, derived by analysing the depth buffer.
- **Pages are cached between frames** and invalidated only by moving objects or a moving light.
- Directional lights use **clipmaps** rather than fitted cascades — incremental update, and an empty clipmap
  level costs almost nothing.

→ **CERID:** the caching insight is the one to take *now*, and it is cheap: our 4 cascade passes redraw the
entire caster set every frame even when nothing in a cascade moved. With instances resident on the GPU and a
per-cascade dirty test, a static cascade can be **skipped entirely**. That is a large win for the 90 ms GPU at
almost no architectural cost, and it composes with the existing texel-snapped fit (a snapped cascade only
changes when it slides a whole texel — which is exactly a cache-invalidation criterion).
Full VSM (virtual pages + clipmaps) is a REN-41-class slice; note it, do not start it inside REN-40.

---

## 5. Visibility buffer — the bandwidth answer, and we already have the pieces

Visibility-buffer rendering stores only **instance + triangle ID** per pixel and defers all material evaluation
to a full-screen resolve, versus a G-buffer's several full-rate render targets. The advantage is memory
bandwidth, and it grows with resolution — it is the reason the technique is standard at 4k+.

→ **CERID:** `ckir_visbuffer.hpp` exists and the bindless material table exists, so this is reachable. But our
1M frame is **not** bandwidth-bound (90 ms GPU against 256k full-detail instanced draws = vertex/geometry
bound), so a visibility buffer would fix a problem we do not yet have. **Skip for REN-40; revisit after 40-C
when LOD has cut the geometry cost and the bottleneck may genuinely move to shading.**

---

## 6. The recipe for 40-A (what I will actually build)

Scope: **frustum culling for the camera and all four cascades, on the GPU, both backends, pixel-identical.**

1. **Persistent instance residency.** Instance payloads and per-instance world AABBs live in GPU buffers,
   uploaded on dirty only (the chunk-run machinery already does this — extend it to the bounds).
2. **One CKIR compute kernel, five frustums.** Reads instance bounds + a frustum block; per instance runs the
   same positive-vertex AABB test the CPU does today (`aabb_in_frustum`), so parity is a *derivation*, not a
   hope. Emits, per (batch × frustum): the compacted instance-index list at a fixed per-batch base, and the
   indirect args (`index_count`, `instance_count`, `first_index`, 0, 0).
3. **Wave-scalarized compaction** — `subgroup_ballot` + `subgroup_ballot_excl_count`, one atomic per wave.
   Bit-exact integer ops, so determinism holds.
4. **The draw side is already done.** `draw_storage_multi_indexed_depth(_only)` consumes the args; the VS finds
   its batch base through DrawIndex → draw table. `first_instance` stays 0 on both backends.
5. **A GPU-WRITTEN COUNT, using each API's real mechanism** (§1): a new `IRasterContext` verb declaring the
   intent — *execute up to N indirect commands, count sourced from this buffer at this offset* — implemented
   with `vkCmdDrawIndexedIndirectCount` on Vulkan and `ExecuteIndirect(..., pCountBuffer, offset)` on D3D12,
   both queried as a device capability at init. The zero-count form is the **named, reported fallback** for a
   device missing the feature, never the default. ⛔ This is the standing rule: use every ability of every API;
   fall back to the backend's own equivalent when it lacks one; never level down to the intersection.

**Gates.** (a) The GPU-culled arm must be **pixel-identical** to the CPU-culled arm at 10k and 100k on BOTH
backends — a cull is a performance change, not a visual one. (b) The drawn-instance COUNT must match the CPU
cull exactly (a count read back and asserted, so a silently-empty cull cannot pass as "fast"). (c) The board
appends to the REN-40 bench file with fps as **median-of-5**.

**What 40-A does NOT do**, stated so it cannot be quietly claimed later: no occlusion culling (§2), no LOD
(§3 — that is 40-C, and the GPU cost stays ~90 ms until it lands), no VSM (§4), no visibility buffer (§5).

---

## Sources

- [Compute based Culling — Vulkan Guide](https://vkguide.dev/docs/gpudriven/compute_culling/) ·
  [GPU Driven Rendering Overview](https://vkguide.dev/docs/gpudriven/gpu_driven_engines/)
- [Stream compaction using wave intrinsics — Interplay of Light](https://interplayoflight.wordpress.com/2022/12/25/stream-compaction-using-wave-intrinsics/) ·
  [Experiments in GPU-based occlusion culling](https://interplayoflight.wordpress.com/2017/11/15/experiments-in-gpu-based-occlusion-culling/)
- [GPU driven rendering in AnKi: a high level overview](https://anki3d.org/gpu-driven-rendering-in-anki-a-high-level-overview/)
- [Two-Pass Occlusion Culling — Milos Kruskonja](https://medium.com/@mil_kru/two-pass-occlusion-culling-4100edcad501) ·
  [Bevy PR #17413: two-phase GPU occlusion culling](https://github.com/bevyengine/bevy/pull/17413)
- [Virtual Geometry in Bevy 0.16](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16/) ·
  [SimNanite](https://github.com/ShawnTSH1229/SimNanite) ·
  [Nanite: Epic's practical implementation of virtualized geometry](https://medium.com/@GroundZer0/nanite-epics-practical-implementation-of-virtualized-geometry-e6a9281e7f52)
- [Virtual Shadow Maps in Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine) ·
  [Virtual Shadow Maps in Fortnite Battle Royale Ch.4](https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4) ·
  [Sparse Virtual Shadow Maps — J Stephano](https://ktstephano.github.io/rendering/stratusgfx/svsm)
- [Triangle Visibility Buffer — Diary of a Graphics Programmer](http://diaryofagraphicsprogrammer.blogspot.com/2018/03/triangle-visibility-buffer.html) ·
  [Visibility Buffer Rendering with Material Graphs — Filmic Worlds](https://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/) ·
  [Bindless Oriented Graphics Programming — Alex Tardif](https://alextardif.com/BindlessProgramming.html)
- [DirectX-Specs: Indirect Drawing](https://microsoft.github.io/DirectX-Specs/d3d/IndirectDrawing.html) ·
  [vkCmdDrawIndirectCount](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDrawIndirectCount.html)
