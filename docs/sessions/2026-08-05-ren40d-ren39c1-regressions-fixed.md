# 2026-08-05 — REN-40-D + REN-39-C1: the two pre-existing GPU-gate regressions fixed (both backends)

After RAF-10 closed, three flagged failures remained from the RAF-8 flip baseline. All three are now fixed and the
full `crd-scene-render-tests` suite is **64/64 (1394 assertions)** GREEN on **Vulkan AND DX12**.

## 1. CKIR `pbr_neutral` / `saturate` / `gamut_compress` lowering (fixed first)
The post tonemap ops fed a **sampled vec4** (the scene colour) into vec3 math (`mix(vec4, vec3)` has no GLSL
overload) → `create_program` returned null. Fix: `post::detail::rgb3(g, colour)` drops alpha to vec3 for
`agx`/`pbr_neutral`/`gamut_compress`, and `saturate` gained a width-robust vec4 branch (preserves alpha; the vec3
path stays node-identical). Full `crd-kir` (52917) + `crd-material-cook` (638) suites green; a both-backend post gate
asserts every tonemap op lowers from a sampled vec4.

## 2. REN-39-C1 — indexed-pull vs multi bit-identity
The command encoder's `MultiStoragePull` required a colour target, so a DEPTH-ONLY multi-pull cascade drew nothing →
empty shadow atlas → self-shadow black. Fix: a `draw_storage_multi_depth_only` verb (Vulkan override + interface
default loop; DX12 uses the default), and the REN-39-C1 gate now discriminates on `multi_indexed_batch_count()`
(index-buffer batches) instead of overloading `pull_batches`.

## 3. REN-40-D — EVSM/MSM moment shadows rendered BLACK (both backends) — the hunt

**Symptom:** `floor 0` (exact 0, both backends) for `soft=Evsm|Msm`; PCF (`soft=Off`) fine (`floor 91`). A regression
— the session log `2026-07-31-ren40d-soft-shadows-closed.md` proves the moment tier passed on both backends before the
RAF band (`aacd5c5`, "renderer backbone").

**The long way round (all red herrings):** `floor 0` reads as a missing WRITE, so the moment fullscreen-colour-layered
transient write got audited exhaustively — image creation + usage, single-slice layer views (`baseArrayLayer`), the
array sampling view, barriers (`VK_REMAINING_ARRAY_LAYERS`), `one_colour_rendering` (`layerCount=1`), the RAF-8
executor adapter vs the inline `record_textured` path (both fail), pass ordering (Kahn topo intact), aliasing (ruled
out with BOTH `persistent` and `no_alias`), and the shader (only an additive shadow-fade changed since the pass). All
correct. A `layers=1` probe "worked" but was a false positive (it also flips `pass_texture_is_depth`, changing the
bind arm).

**The lever that cracked it:** a probe in `atlas_sampler_for` printing `is_depth`/`format`, correlated with a probe on
the scene pass's resolved `pass_texture` — the **PCF forward called `atlas_sampler_for`; the MSM/EVSM forwards never
did.** The atlas verb was never reached, so the moment atlas was never bound.

**Root cause:** the RAF-8 `TranslatingCommandEncoder` classifies a scene draw's textures by SAMPLER KIND —
`shadow_atlas_from` returned a texture only if a `ComparisonSampler` was present, and `map_texture` grabbed any
non-depth `SampledTexture`. Correct only while every atlas is a DEPTH atlas. The EVSM/MSM moment atlas is a **COLOUR
(RGBA16F) array read through a PLAIN linear sampler** → `shadow_atlas_from` rejected it, `map_texture` STOLE it → it
bound at the MAP slot (1) while `forward_csm` read it at the ATLAS slot (4) → unbound slot-4 read → 0 → black.

**Fix (`engine/gpu-context/src/command_encoder.cpp`):** recognize scene textures by the DECLARED SLOT the render-graph
writes — `MAP = slot 1`, `ATLAS = slot 4` (`ResourceBinding.slot`, set by `bind_map`/`bind_atlas`) — never by sampler
kind or `is_depth()`. The downstream verb still picks comparison-vs-linear from the texture's own format
(`atlas_sampler_for`), so one slot serves both atlas kinds. Two supporting moment fixes were also load-bearing (the
convert produces the moments): the fullscreen convert reads raw depth through the fullscreen sampler seam — sampler
`(0,6)`→`(0,2)` + plain (non-shadow) texture, driven by `depth_as_float=true` on the pass; and
`pass_texture_comparison` (plain sampler for a colour atlas, comparison for a depth one) so `bind_atlas` declares the
correct sampler kind.

## Verification
- Moment gate: Vulkan 23 assertions, DX12 20 assertions — both green (contrast, floor-not-dimmed, EVSM/MSM penumbra
  widths all pass, so the moments are CORRECT, not merely non-zero).
- Full `crd-scene-render-tests`: **64/64, 1394 assertions**, both backends — no regression (PCF/CSM/RAF-9/RAF-10 green).
- LLVM-20 clang-tidy: all 12 changed source files + headers clean, per-slice.

## Scar
[[feedback_command_encoder_recognize_scene_textures_by_slot_not_sampler]] — an encoder/verb that special-cases
"shadow" vs "map" vs "atlas" must key off the DECLARED SLOT (the contract), not the sampler kind or format (data).
And: when a resource reads 0 despite its producing passes drawing, probe the READ's binding path with a validated
control (the PCF arm) BEFORE auditing the write.

## Files
`engine/gpu-context/src/command_encoder.cpp` (the fix) · `engine/kir/include/crd/kir/ckir_technique.hpp` (convert
sampler `(0,2)`) · `engine/kir/include/crd/kir/{ckir_post,ckir_nodes}.hpp` (CKIR post) · `assets/frame/forward_csm_moment.frame.toml`
(`depth_as_float`, comment) · `engine/frame-cook/src/{frame_runtime,frame_compose}.cpp` · `engine/render-graph/{src/frame_graph.cpp,include/.../frame_graph.hpp}`
· `engine/gpu-context/include/crd/gpu/raster_context.hpp` · `engine/gpu-context-{vulkan,dx12}/src/*_raster_context.cpp`
· `tests/scene-render/test_scene_render_gpu.cpp`.
