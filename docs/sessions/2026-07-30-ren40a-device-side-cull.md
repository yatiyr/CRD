# 2026-07-30 — REN-40-A: the device-side cull, closed live on both backends

**Row 143 / 40-A in `docs/detours/D-007-gpu-program-system.md`. Board:
`docs/bench/2026-07-29-ren40-million-instance-baseline.md` (REN-40-A section).**

---

## What shipped

The frustum cull for the camera **and all four shadow cascades** now runs on the device, and the draws take their
counts from device memory. Under `SceneRenderer::set_gpu_cull(true)` the CPU does not cull at all.

It is one authored frame graph, `assets/frame/forward_csm_gpu.frame.toml`:

```
cull_reset · cull_view0..4        kind = "compute", each walking a DRAW LIST
csm_cascade × 4                   for_each = "light.0.cascades"
forward · post
```

13 device passes, **one submission**. A compute pass that declares a draw list dispatches **once per item**,
binding that item's own scene buffer at slot 0 and its own indirect command at slot 1 — which is what lets ONE
authored kernel (`scene_cull_compact.crdv`, cooked into five per-view variants) cover every mesh group without the
asset naming any of them. The grid comes from the item, because a pass cannot know a group's instance count.

**The compacted lists are not new resources.** Each kernel writes its list back into the instances buffer at
`visible_off + view_index * capacity` — the CPU cull's layout verbatim — so **not one vertex program changed**
between the CPU and device paths.

New engine surface:

| | |
|---|---|
| `draw_storage_multi_indexed_indirect` | the GEOMETRY half of the GPU-written draw (colour + depth, both texture slots), Vulkan + DX12 |
| `CullDesc::frustum_off` | which view's clip matrix this dispatch tests against — a cook-time constant |
| `CullDesc::base_word` | where the group's buffer base lives inside the args params block |
| `SceneRenderer::read_gpu_cull_counts()` | what the DEVICE decided, read back — per-view survivors, index_count, first_index, and the CPU's answer beside it |
| `set_gpu_cull_verify()` | keep the CPU cull alive purely for that comparison (a GATE mode) |
| sandbox `--fixed-dt` / `--gpu-cull` / `--gpu-cull-verify` / `--frame` / `--no-bvh` | see below |

## The numbers

| backend | 1M instances | CPU render | GPU |
|---|---|---:|---:|
| Vulkan | CPU cull | 281.32 ms | 85.86 ms |
| Vulkan | **device cull** | **120.82 ms** (2.33×) | 86.82 ms |
| DX12 | CPU cull | 278.94 ms | 86.26 ms |
| DX12 | **device cull** | **117.24 ms** (2.38×) | 86.42 ms |

Frame **BIT-IDENTICAL** to the CPU-cull arm on both backends: 0 of 921600 pixels differing, max channel delta 0.
Per-view survivor counts read back and asserted EQUAL to the CPU cull's on both. GPU time did not move, and that is
expected — a frustum cull changes who decides, not what is drawn. **That wall is LOD (40-C/40-D).** The largest
remaining CPU term is now `sync` (77–98 ms at 1M), almost all of it the O(entities) extract walk — 40-B.

---

## Five defects, and what each one taught

Every one of them rendered a plausible frame with a clean log.

### 1. A pass that READS what it WRITES makes the graph cycle — and the failure was silent

Six compute passes accumulate into `cull_args`. Declaring it as `reads` **and** `writes` is a genuine cycle (every
view would depend on every other view's write), which the cooker correctly refused. **A read-modify-write is a
WRITE**, ordered against the previous writer by declaration order; the hazard is covered by the dispatching verb's
compute-write barrier.

Removing the reads was not enough: the *executor* imports **every draw item's vertex-pull buffer as a graph READ**,
and a cull pass walks that same list to find the buffers it compacts into. So the pass was writer and reader of one
handle. Rule now: **if a pass already declared a buffer as a write, do not also declare a read of it.**

⛔ The expensive part was the silence. `IFrameGraph::build()` returned a bare `false` that nobody reported, so the
frame showed the canvas's **previous contents** — complete-looking, missing exactly the passes that mattered.
`SceneRenderer` now logs `frame graph '<name>' failed to BUILD (N passes) — nothing was drawn`.

### 2. Every cascade dispatch culled against the CAMERA

The kernel read its view-projection from `header.view_proj` unconditionally, so all four cascade dispatches produced
the camera's visible set. `CullDesc::frustum_off` now names the view's own matrix (`light_vp + c*16`, the same words
`frustum_planes(cascades.light_vp[c])` feeds the CPU cull) as a cook-time constant. The Y-flip the emitters apply to
a light matrix is invariant here: negating the Y row swaps the top and bottom plane *expressions* and leaves the
six-plane SET, and therefore every AABB verdict, alone.

### 3. `bounds_off = 104` — the scar of this slice

The asset declared header word 104 for the world-AABB section. The engine's is **102**; 104 is where the **light
record** starts. So the kernel tested boxes built from a light colour — and:

- the frame rendered, geometry all present, no validation error;
- the counts came back *plausible* (1918 / 0 / 258 / 1959 / 1963 of ~2000);
- a readback comparing the **device's copy of the AABBs** against the CPU's reported **0 of 646 differ** — because
  the boxes were uploaded perfectly and simply never read.

What found it was the only measurement that could: **the device's per-view survivor counts against the CPU cull's
for the same frame** — 1918 vs 1377. The host now **REFUSES** a cull asset whose declared header words are not the
engine's, naming both numbers. Lesson, in order: compare **outputs**, then inputs, then the maths. Verifying the
inputs first is what let "the AABBs are bit-identical" read as "the boxes are fine".

### 4. A consolidated group's header is not at word 0

Under REN-38 scene-buffer consolidation a group's region sits at `region_base` and its section offsets are
region-relative — the vertex programs add the base from the draw table via DrawIndex, which a kernel does not have.
`CullDesc::base_word` hands the base to both kernels through a 16-byte params block at the head of the args buffer,
and every header read and every offset read out of the header became `base + …`.

### 5. `VkPhysicalDeviceVulkan12Features` cannot be chained here

`drawIndirectCount`'s feature bit lives only in that aggregate, and the aggregate is **mutually exclusive** with the
individual promoted structs this device already needs — DescriptorIndexing for bindless, BufferDeviceAddress for
RT/DGC (**VUID-VkDeviceCreateInfo-pNext-02830**, which validation reported the moment the sandbox ran). The other
legal way to satisfy VUID-…-None-04445 is to enable the **`VK_KHR_draw_indirect_count` extension**, which carries no
feature struct and therefore no conflict. That is what ships.

---

## ⛔⛔ The measurement mistake that cost the most

I compared two screenshots taken with `--screenshot-at 5.6` and concluded **"the device-cull arm has lost its
shadows"**. It had not. The sandbox clock was the wall clock, the two arms run at different frame rates, so the two
captures sat at **different camera poses** — a ~50% pixel difference with nothing to do with the change. I then
spent a long stretch hunting a regression that did not exist: made the atlas persistent, dumped the built-in frame
to disk to A/B the asset, added `--frame`, checked transient aliasing, re-read the device chain.

What settled it was a **statistic, not a look**: mean luminance and dark-pixel fraction across all four captures
agreed to within 0.2% (166.7–166.9 / 3.57–3.66%). Adding **`--fixed-dt <ms>`** (clock = presented-frame counter × dt)
made the run deterministic, and the same two arms came back **bit-identical**.

**Rule: any harness used for pixel A/B needs a deterministic clock, and a visual difference must be quantified
before it is believed.** "Shadows gone" should have been "3.6% dark pixels in both, so no."

---

## Gates

- **Vulkan** `[ren40]` — 3 cases, 83 assertions: the count-buffer draw (depth-only), the **new geometry indirect
  draw**, and the compacting cull kernel reproducing the CPU cull exactly (same count AND same set).
- **DX12** `[ren40]` — 2 cases, 54 assertions: the count-buffer draw and the geometry indirect draw, in D3D12's own
  24-byte / arg-offset-4 command layout.
- Live: bit-identical pixels + equal per-view counts on both backends (above).
- Module tests green: frame-cook 391, scene-render 791, vertex-cook 424.

## Still open (named, not deferred quietly)

- **`count_buf` is passed `nullptr`** in the live path: with one command per group per view (`max_draws = 1`) a count
  word buys nothing. The ability is real and gated on both backends; it becomes load-bearing in 40-B when all groups
  share one command buffer.
- **Compaction is per-survivor**, not wave-scalarized. The fast form was implemented first and the parity gate
  rejected it (`~ballot` phantom lanes — eight slots unwritten with the count still right). Correct first.
- **GPU ~86 ms at 1M** — no LOD (40-C/40-D). **`sync` 77–98 ms** — the extract walk (40-B).
