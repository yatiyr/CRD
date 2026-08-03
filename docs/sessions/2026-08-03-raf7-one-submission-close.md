# Session 2026-08-03 (later) — RAF-7 closed: one-submission frame recording + all 4 frame-graph-shaped kinds gated

> Re-entry pointer: `context.md` §"STATE AT 2026-08-03 (LATER)". Band: D-007 "RAF band". Mission: §7 / Gate 7.
> Prior half: `docs/sessions/2026-08-03-raf-substrate-through-frame-graph.md` (RAF-0…7 substrate). Everything below is
> **green (VK+DX12), LLVM-20 tidy-clean, and uncommitted** (user controls commits).

## The pivot (the important part)

The session opened to close "the ONE bounded item left in Phase 7": GPU-gate the 4 frame-graph-shaped command kinds
(**MRT · indexed-indirect · comparison-sampler/shadow · bindless**) through `crd-render-graph`. The RAF-2-era memory
scar framed this as needing a **coherent transient-set allocation** capability (`draw_storage_mrt` "renders attachment
1 black with standalone targets"). The advisor (`arch-review`) + a code read **refuted that twice over**:

1. Not a co-allocation problem — Vulkan dynamic rendering with N independent same-size views and DX12
   `OMSetRenderTargets(n, rtvs, FALSE)` are both legal; the verbs already bind N independent views.
2. Not a missing synchronous verb body either (my first fix — reverted). The verbs are **frame-recording verbs by
   design**: they record into the frame's command buffer and the **readback is the frame graph's job**.

The user caught the deeper issue: *"if we delete these verbs in RAF-12, why introduce sync scaffolding that gets
deleted?"* Correct. The real gap was that `crd-render-graph::execute()` drove the encoder **synchronously** (per-verb
submit+wait — itself scaffolding). **The fix is one-submission FRAME RECORDING** (mission Gate 7 "one submission where
expected"), where all 4 kinds work via their existing bodies with zero scaffolding.

## What shipped

### `execute_frame` — one-submission device execution (`crd-render-graph`)
`crd::rendergraph::execute_frame(compiled, tmpl, records, table, programs, raster, alloc, diags, &submit_count)`:
builds a gpu-context `IFrameGraph`, imports every resolved resource (color/depth via `import_target`, textures via
`import_texture`, buffers via `import_storage`), declares each pass's reads/writes from its payload, and records each
pass by invoking its RECORD function through a **frame-recording** `ICommandEncoder`. The gpu-context frame graph owns
the ONE command buffer, the cross-pass barriers, transient handling, and the end-of-frame readback. `out_submit_count`
returns the actual submission count (the checkable Gate-7 proof). The device-free `execute(...encoder...)` path is
kept for the architecture gate's mock encoder.
- **Graph texture resolution** added so bindless/shadow resolve textures: `ResolvedResource::texture` +
  `RecordContext::texture(slot)` (`SlotResourceKind::Texture` already existed).
- New diagnostic `DiagCode::ExecutionFailed` (render-asset-core) for a device build/record failure.

### The 4-kind gate (`crd-render-graph-gpu-tests`, both backends) — **167 assertions**
Test executors registered WITHOUT an engine-enum edit (the RAF-6 app-executor precedent), each with a distinguishable
per-slot assertion so a dropped attachment/binding fails loudly:
- **2-pass scene→copy** switched to `execute_frame`; asserts `submit_count == 1`. (In one submission `scene` is an
  intermediate the copy consumes → ends TRANSFER_SRC → not independently read back; the proof is the final `copy`.)
- **MRT** — `build_gbuffer_two_output_fs`: att0 = RED, att1 = GREEN (kills the "attachment 1 black" scar directly).
- **bindless** — `build_textured_vs` + `build_bindless_fs` + 2 solid textures: left = tex0 (RED), right = tex1 (GREEN).
- **shadow** — a local scene-shadow FS sampling the atlas at bindings 4/5 (the `draw_storage_shadowed_depth` layout,
  not the simple `draw_shadow` 1/2) + `create_depth_texture` atlas: left lit (white), right shadowed (black).
- **indexed-indirect** — `build_indexed_probe_vs` (positions by index value; no storage read → DX12-SRV-safe) + a
  device args buffer. ⚠ the one genuine per-backend difference: DX12's `ExecuteIndirect` command signature prepends a
  DrawIndex root constant (args = 6 u32 / 24 B), Vulkan reads a bare `VkDrawIndexedIndirectCommand` (5 u32 / 20 B);
  the test formats the args per backend via an `is_dx12` flag. The standard root sig already has the DrawIndex root
  constant (DX12 param 6), so a plain program works.

### Drive-by fixes (both legit, both re-verified)
- **DX12 `draw_storage_mrt` PSO miswiring** — the frame-recording body built its PSO with `samples=n` /
  `conservative=has_depth` (the code's own comment flagged it as "only never bit because MRT ran depthless"). Now the
  correct `samples=1` / `conservative=false`. Existing DX12 frame-graph MRT test (38-A1b) re-run: no regression.
- **`command_model.hpp` lint marker** — `DepthStencilAttachmentDesc::clear_depth` was a bare-float physical field the
  `crd-no-untagged-physical-numeric` guard flagged (pre-existing; git-clean file); added
  `// crd-lint-allow-untagged-physical: normalized device depth [0,1], a raw API scalar`.

## Discipline / DoD
`crd-render-graph-gpu-tests` 167 (VK+DX12), all raf7 ctest 7/7, DX12 frame-graph MRT regression 45. LLVM-20 tidy-clean
on every touched file. Guards: no-untagged-physical GREEN (after the marker), ascii/no-std GREEN. (⚠ `simd-emission`
needs the math test obj — a full-build guard, not built in this targeted run; unaffected by these changes.) The
`feedback_draw_storage_mrt_needs_coherent_frame_graph_transients_not_standalone_targets` memory scar is **struck in
place** with the corrected root cause; MEMORY.md hook updated.

## Next
RAF-7 is closed. **RAF-8** (scene-render → orchestration) is next — the first LIVE-code phase. ⚠ `execute_frame`
currently runs the graph ON TOP OF the gpu-context `IFrameGraph`; RAF-12 unifies the two frame graphs (the old
FramePassKind machinery is what RAF-7's executor model replaces).
