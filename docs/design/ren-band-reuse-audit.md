# REN band — the REUSE AUDIT (all 35 rows)

**Status**: pass 1, 2026-07-25. User-directed: *"fully audit REN-3 and other REN band slices before we start,
look at all of them, make deep researches, go find what we have done in our engine and make those slices
better."*

**Method** (SANITY #8 — search the engine before you build): for every row, grep the tree for the capability,
classify each gap as **REUSE** (exists, device-proven) / **WIRING** (exists, not connected) / **NEW**, and
record file-level evidence. The trigger was REN-3's sky section: what read as "new work" turned out to be four
LUT kernels already dispatching oracle-green on Vulkan. **That mistake is the default failure mode of this
band** — the CKIR shader libraries are years ahead of the device/renderer code that consumes them.

## ⛔ THE ACTUAL FINDING OF PASS 1 — read this before trusting anything below

**The D-007 rows already contain their own reuse audits, and they are more accurate than my greps were.**
Pass 1 set out to find what the rows had missed. It mostly re-derived what the rows already say, and it was
outright wrong twice. Three worked examples, kept visible on purpose:

| I claimed | reality |
|---|---|
| REN-5 confuses **V**SM with **E**VSM — "a trap" | **False.** `vsm_clipmap_level` (`ckir_lighting.hpp:1066`) + `vsm_page_coord` (`:1073`) exist, are bit-exact, and are gated (`tests/gpu-shared/ckir_raster_triangle.hpp:1926`). The row says exactly this. I grepped a narrow window, hit `evsm` first, and stopped. |
| REN-35's CKIR backends exist — "a large correction" | **The row already says it**, verbatim: *"⭐ the shader half is ALREADY DONE: CKIR emits WGSL (`ckir_wgsl.hpp`) and MSL (`ckir_msl.hpp`) TODAY — this slice builds the DEVICE half those emitters have been waiting for."* |
| REN-6 morph targets: "nothing exists" | **Wrong.** The row states the ANIM weight tracks *"already PARSE at GEO-8"* and that `palette_to_dual_quats` shipped there. |
| REN-4 is "smaller than written" | **Wrong.** The indirect *primitives* exist, but REN-4's real content — two-phase occlusion culling, the HiZ pyramid, meshlet cooking — is genuinely new. I sized the row from one clause of it. |

**Method failure to carry forward:** grep the whole tree for **the row's own named symbols**, and **read the
whole row**, before declaring it wrong. A partial grep against a partially-read row is far more likely to be
wrong than the row is. The rows were written with the code in front of the author.

**What this means for the band:** a blanket re-audit is low value. The rows do not need correcting; they need
(a) the **cross-row seams** that changed today declared, and (b) per-slice specs written just-in-time, each
opening with a reuse audit done properly. Those are the surviving deliverables, below.

**Where the original premise DID hold:** REN-3's sky. That correction was real and worth the whole exercise —
four Hillaire LUTs already dispatching oracle-green on Vulkan, which the row did *not* say.

**⛔ CKIR is binding for everything added here** (ADR-0101: the IR is the source of truth; backend languages
are outputs only, never authored or stored). This is also *why* reuse is so high — the libraries are portable
IR, so "make it work on DX12" is usually *running* it, not porting it.

---

## REN·A — the device band (rows 98–106, plus the A-scoped additions 132/133/136/137/138)

| row | slice | verdict | evidence + correction |
|---|---|---|---|
| 98 | REN-1 frame graph | ✅ **DONE** | Vulkan + DX12, one submission, transient aliasing. Bench `2026-07-24-ren1-frame-graph-batching.md`. |
| 99 | REN-2 RTT + textures | ✅ **DONE** | Both halves + SceneRenderer + live gate. |
| 100 | **REN-3** lighting/shadow/sky/AA | ⬜ next | Spec v2 `ren-3-lighting-shadow-pipeline.md`. **Corrected during this audit** — the four Hillaire LUTs already dispatch oracle-green on Vulkan (`test_vulkan_context.cpp:5755/5809/5872/5952`, maxabs 6.25e-5 / 1.46e-5) with a `[.gi-bench]` board, so 3.5 shrank from "the increment with real new work" to "mostly wiring + IBL generation". |
| 101 | REN-4 GPU-driven pipeline | ⬜ **SMALLER THAN WRITTEN** | ⭐ The GPU-driven *primitives already exist and are gated*: `IRasterContext::draw_mesh_indirect` (`raster_context.hpp:399` — `vkCmdDrawMeshTasksIndirectEXT` / DX12 `ExecuteIndirect(DISPATCH_MESH)`), `IComputeContext::dispatch_indirect` (`compute.hpp:102`), device-generated command streams (C5, incl. per-sequence pipeline switch), and B4's *"GPU-driven indirect meshlet dispatch — a compute cull writes the dispatch count"* is a PASSING Vulkan test. **REN-4 is assembling an existing GPU-driven loop into the frame graph + SceneRenderer, not building the capability.** Re-scope the row accordingly. |
| 102 | REN-5 **VSM** | ⬜ **row is ACCURATE — no correction needed** | ⚠ **This entry was WRONG in the first draft of this audit** and is corrected here as a worked example of the method failing. I grepped a narrow window of `ckir_lighting.hpp`, hit `evsm_warp`/`evsm_shadow` (**Exponential Variance** SM — a different technique), concluded the row confused it with **Virtual** SM, and flagged a "name collision trap". **There is no trap.** `vsm_clipmap_level` (`ckir_lighting.hpp:1066`) and `vsm_page_coord` (`:1073`) are real UE5-VSM clipmap helpers, bit-exact and exercised by a gated test (`tests/gpu-shared/ckir_raster_triangle.hpp:1926-1928`) — exactly as the row already states. The genuine remaining work is the **device** realization (page table · GPU feedback buffer · on-demand page allocation), which is what the row says. **Lesson for pass 2: grep the whole tree for the row's OWN named symbols before concluding a row is wrong — the rows are more accurate than a partial grep suggests.** |
| 103 | REN-6 morph targets + GPU DQS | ⬜ **HALF EXISTS** | Dual-quaternion skinning is done and gated (B8-j *"IR skinning (linear-blend + dual-quaternion)"*). **Morph targets: nothing** — no morph in `ckir_*`, `crd-anim`, or the resource layer, and the cooked 48-byte vertex has no morph-delta channel. So this row is "DQS ✅ / morph = new, incl. a cooked resource-format change". Split it. |
| 104 | REN-7 timeline → real-scene binding | ⬜ **PARTLY DONE ALREADY** | The sandbox **already** drives its camera from a `TimelineResource` (`sandbox/src/main.cpp:70` `build_camera_timeline`, rational-time sampled per frame, GEO-9). What is missing is binding timeline channels to *scene properties in general* — which is really a `crd-reflect` (REN-11) path-addressing problem. **Note the dependency; it is not stated in the row today.** |
| 105 | REN-8 GPU profiler + present + batching | ⬜ **SEAM EXISTS** | `crd-perf` already defines the backend interface — `IGpuProfilerBackend` with `begin_span(void* cmd_buffer, NameId)` / `end_span` / `begin_frame` / `end_frame` (`gpu_scope.hpp:72-80`) and a test double (`tests/perf/test_gpu_scope.cpp`). **Only the timestamp-query implementation is missing**, plus direct-to-backbuffer present and overlay batching. Much smaller than it reads. |
| 106 | REN-9 visual frontier restored | ⬜ | The "restore" list should be enumerated against what the retired renderer actually did; **not yet audited in depth — pass 2.** |
| 132 | REN-29 POST/HDR/COLOR | ⬜ **MOSTLY REUSE** | `ckir_post.hpp` = 9 gated entry points (`ev100_from_luminance`, `exposure_from_ev100`, **AgX**, Khronos PBR-Neutral, `srgb_encode`, …); `ckir_bloom.hpp` = 12 more (incl. an FFT-convolution bloom path). ⚠ **REN-3.4 now takes the HDR-target + exposure + tonemap + encode chain** (a real BRDF without a tonemap is a visual regression, so it cannot wait). **The rows overlap and the seam must be stated:** REN-3.4 ships the minimum correct chain; REN-29 keeps bloom, the histogram auto-exposure reduction, colour management/OCIO-class transforms, and the rest of the post stack. |
| 133 | REN-30 picking/selection | ⬜ **SUBSTRATE EXISTS** | B4-vis-4 *"HW-raster visibility buffer writes SV_PrimitiveId per pixel"* is a passing Vulkan test, and `ckir_visbuffer.hpp` has the full Karis-2021 software rasterizer. ID-buffer picking is **wiring on a proven substrate**; the editor-side selection model is the new part. |
| 136 | REN-33 bindless + visibility-buffer path | ⬜ **BOTH HALVES DEVICE-PROVEN** | B2-d *"IR bindless texture array (dynamic index)"* and B4-vis both gate green. ⚠ **REN-3.3 now consumes bindless** for per-instance materials — state the seam so the two rows don't both claim it. |
| 137 | REN-34 NPR / stylized | ⬜ **SEAM EXISTS** | B5-c ships a *shading-model tag* with Gooch + a masked alpha domain; B6-d ships MaterialX `gooch_shade`. The **material-model seam is already in the IR** — REN-34 is widening it, not inventing it. |
| 138 | REN-35 cross-platform backends | ⬜ **CKIR BACKENDS ALREADY EXIST** | ⭐ `engine/kir-webgpu`, `engine/kir-metal`, `engine/kir-hip`, `engine/kir-cuda` are **real modules in the tree** (they appear in the CMake configure; HIP/Metal skip on this host by design). `ckir_wgsl.hpp` / `ckir_msl.hpp` emitters exist and are gated. **What is missing is the `gpu-context` DEVICE layer for those targets, not the shader translation.** That is a large correction to how this row reads. |

## REN·B — the interactive frontier (rows 113–131)

**Headline: these modules do not exist.** `engine/reflect`, `engine/font`, `engine/ui`, `engine/vector`,
`engine/text` — **none present**. No MSDF, no shaping, no bidi anywhere in the tree. So unlike REN·A, the B
rows are honestly sized. The reuse that *does* exist is worth naming precisely:

| row | slice | verdict | evidence |
|---|---|---|---|
| 113 | REN-10 ECS audit + UI-world | ⬜ **correctly scoped as an AUDIT** | `engine/scene` is substantial (30 headers / 18 sources) and `component.hpp:33` already mentions *"archetype identity and observer matching"*, so the observer hook may be partially present. The four named gaps (ordered child relations · observers · runtime-registered types · cross-world handles) each need their own probe — **pass 2**. |
| 114 | REN-11 crd-reflect | ⬜ **green-field** | No `engine/reflect`. This is the band's centerpiece and is honestly sized. **REN-7 depends on it** (timeline → property binding) — add that edge. |
| 115 | REN-12 crd-font | ⬜ **green-field** | No font code. Sized correctly (month-class components). |
| 116 | REN-13 MSDF glyphs | ⬜ **green-field** | No MSDF. |
| 117 | REN-14 shaping/paragraph | ⬜ **green-field, the hardest row** | No shaping/bidi/UAX. Correctly flagged month-class + laddered. |
| 118 | REN-15 crd-vector | ⬜ **green-field** | No vector rasterizer. Correctly flagged month-class. |
| 119 | REN-16 UI paint | ⬜ **REAL REUSE — understated** | ⭐ `engine/draw/include/crd/draw/ckir_draw.hpp` already ships crd-draw's shader suite **as CKIR graphs** — AA lines, solid triangles, infinite grid — with an established **storage-buffer data contract** (header words + tightly-packed instances, f32-as-u32 bit patterns, vertex-pulled, no vertex-input state, CPU-oracle-gated). That is *exactly* the batched-CKIR-primitive architecture REN-16 describes. **REN-16 should extend this contract, not invent one.** |
| 120-131 | REN-17…REN-28 | ⬜ | Depend on 11–16; sized on green-field assumptions that this audit confirms. `crd-timeline` + `crd-anim` + `hesap-interp` (akima/cubic_spline/keyframe/…) exist and are the substrate REN-19 (UI animation) and REN-24 (sequencer + curve editor) build on — the "ONE curve engine" claim checks out. `crd-imgui` stays the debug overlay as ADR'd. |

---

## ⚠ Method warning, learned the hard way in pass 1

The first draft of this audit "found" a name-collision trap in REN-5 and called it the highest-value
correction. **It was a false alarm** — I had grepped a narrow window, matched `evsm`, and never searched for
the symbols the row itself names (`vsm_clipmap_level` / `vsm_page_coord`, both present and gated). Before
declaring any row wrong: **grep the whole tree for the row's own named symbols.** The rows in D-007 are
written by people who had the code in front of them; a partial grep is far more likely to be wrong than the
row is. Every entry below has been re-checked against the row's named symbols.

## The SURVIVING deliverables (everything else is withdrawn)

Four items survived the re-check. These are things the rows genuinely do **not** say — three of them because
REN-3's scope changed *today*, which is precisely the kind of drift a row cannot anticipate.

1. **REN-29 ↔ REN-3.4 — a NEW overlap, must be declared.** REN-3.4 now takes the HDR-target + exposure + AgX +
   encode chain, because a real BRDF without a tonemap clips and is a visual *regression* — it cannot wait for
   REN-29. Seam: **REN-3.4 = the minimum correct HDR chain; REN-29 keeps bloom, the histogram auto-exposure
   reduction, and colour management.** Without this both rows claim the same work.
2. **REN-33 ↔ REN-3.3 — a NEW overlap.** REN-3.3 consumes B2-d bindless for per-instance materials (REN-2
   explicitly deferred those to "a bindless follow-up"). State it so REN-33 doesn't re-claim it.
3. **REN-16 ← `crd-draw/ckir_draw.hpp`** — the one reuse find the rows don't carry. crd-draw already ships its
   shader suite as CKIR graphs with an established storage-buffer contract (header words + tightly-packed
   instances, f32-as-u32 bit patterns, vertex-pulled, no vertex-input state, CPU-oracle-gated). That IS
   REN-16's described architecture. **Extend that contract; do not invent a second one.**
4. **REN-7 → REN-11 dependency.** The sandbox already drives its camera from a `TimelineResource`
   (`sandbox/src/main.cpp:70`). Generalizing timeline→*any scene property* is path-addressing, i.e. a
   `crd-reflect` capability. The edge is real and unstated.

**Withdrawn:** the REN-4, REN-5, REN-6, REN-8, REN-30, REN-34 and REN-35 "corrections" — the rows already say
it, or I was wrong. Left visible above rather than deleted, so the next reader sees the failure mode too.

## Honest gaps in this audit (pass 2)

- **REN-9** (visual frontier restored) — needs the retired renderer's feature list enumerated before it can be sized.
- **REN-10** — the four ECS gaps need individual probes; I checked for their *names*, not their *semantics*.
- **REN-17…REN-28** — audited only for module existence, not for design-level reuse against `crd-scene`/`crd-draw`.
- Nothing here has been re-verified against the **retired** `rhi`/`renderer` code, which may contain further
  prior art worth lifting (ADR-0032's frame graph already was).
