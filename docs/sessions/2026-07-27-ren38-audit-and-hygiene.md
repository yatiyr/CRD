# 2026-07-27 (second session) — REN-38 FULL-ARC AUDIT + THE HYGIENE PASS

> **User-directed: "check for gaps and scars for the whole REN-38 arc … fix all the gaps, I don't want any
> gaps … make the system as hygienic as possible."** The audit verified all 53 closed rows in code, found
> 8 gaps, and this session closed them. Two of the fixes found and killed REAL latent defects in shipped
> code paths; the new checkers caught five more while being wired in. Every fix carries its own gate.

---

## The audit (verified before fixing)

All 53 ✅ rows of REN-38 A–F spot-verified in code: the deletions are real, every advanced `FramePassKind`
records with the clear-then-LOAD discipline, DX12 `trace_rays` exists (A16 genuine), `VariantKey::vertex`
honestly open, no stray CKIR builders outside `ckir_draw.hpp` (=F7). Frontier check (O3DE Atom `.pass`
assets, DXR 1.2 OMM/SER) confirmed the vocabulary's shape and located the state-cluster gap below.

## Gap #1 → fixed: the parse-into-reused-descriptor scar lived in FOUR more parsers

`parse_technique_toml`, `read_technique`, `parse_frame_toml`, `read_frame_graph` all APPENDED into a reused
descriptor (and the frame parser kept stale `memory_budget_bytes`/`fallback` scalars the new file never
wrote). All four now reset first; gated by "a REUSED descriptor holds ONLY the second asset" in both
technique-cook and frame-cook (TOML and blob paths).

## Gap #2 → fixed: repo hygiene

- `TempPack` RAII + **`platform::fs::temp_directory()`** (new engine primitive): every scene-render gate pack
  now lives in the OS temp dir and is removed on unwind. The `sr_*_pack_*.crdr` repo-root crop is deleted;
  `.gitignore` keeps a belt-and-braces pattern.
- `frame_asset.hpp`'s stale "the DX12 RT-pipeline half does not exist" comment corrected (A16 shipped it).

## Gap #3 → built: THE SHAPE CHECKER (`ckir_shape.hpp`) — and its first three real catches

`nodes::detail::bin`'s mismatched-vector arm deferred to "the shape checker", **which did not exist** — the
38-E7 mechanism (cook returns a valid id, the SHADER fails to compile far from the asset). The checker
mirrors the ORACLE's exact read semantics (strict elementwise width equality — a narrower operand is an
oracle OOB read and a silent GPU broadcast), plus VecComp/Swizzle bounds, Select arms, constructor lanes,
and the SAMPLE rules (uv width from the texture's dim+arrayed; comparison-sampler ⇄ `tex_sample_cmp`
pairing — keyed on the SAMPLER node, exactly as the GLSL emitter is). All three cookers refuse a
shape-invalid graph **by name** (`ShapeIssue` out-params thread the offending node + reason).

**It caught, on its first run:**
1. ⛔⛔ **The PCSS blocker search read depth through the COMPARISON sampler** — `texture(sampler2DArrayShadow,
   vec3)` is an overload that does not exist; the cook succeeded and the shader could never compile. PCSS now
   requires a declared **`shadow_plain_sampler`** (`lighting_needs_plain_shadow_sampler`), absence FAILS by name.
2. ⛔⛔ **Contact shadows fed SCALARS to the 4-tap `lt::contact_shadow`** — the helper swizzles lanes 0..3 of
   its inputs, so the shipped E6 contact term read lanes 1..3 of a 1-wide value. The close gate measured node
   counts, so it cooked green. The marched samples now pack four to a vec4 (tail padded by repetition).
3. The light-cook test rig declared ONE plain 2-D texture for EVERY binding slot — mistyped bindings that HID
   defect 1. The rig now types per slot from the declaration (`lighting_shadow_is_comparison`), the live-path
   mirror. The five-filter distinctness gate now compares CONTENT (serialized graphs), not node counts —
   PCF and MSM collided on count by coincidence.

## Gap #4 → fixed: the 38-D4 varying contract runs on the LIVE path — and caught a real mismatch

`verify_varying_contract` was test-only. Now: `fs_varying_requirements` derives a cooked fragment graph's
REAL read set (locations, widths, interpolation — nameless, matched by location), and `init_programs`
verifies every (VS, FS) pair at program creation, failing with a logged reason.
⛔⛔ **First run refused the shipped renderer**: the VS declared the tint FLAT while the material's `geomcolor`
StageIn declares SMOOTH — a cross-stage interpolation mismatch (spec-undefined) that renders correctly only
because a per-instance constant interpolates to itself. The tint is now SMOOTH in the embedded pack AND the
shipped `.crdv`s.

## Gap #5 → built: THE PASS-STATE VOCABULARY (depth-write · depth bias · face cull · stencil)

`PassRasterState` (gpu header) + per-pass asset fields (`depth_write`, `depth_bias{,_slope,_clamp}`,
`face_cull`, `front_face`, `stencil*` — closed sets, named rejections incl. `BadStencilValue` for a ref/mask
past 8 bits) + parse/emit/blob + executor `set_pass_state` + both backends:
- **Vulkan**: all dynamic state, applied inside `set_draw_state` (the A1g ordering scar) and reset at every
  pass boundary (the B8 sampler discipline).
- **DX12**: PSO state → the state joins the PSO cache identity **exactly, member-by-member** (never hashed);
  `OMSetStencilRef` after every pipeline bind. Defaults = the historical hardwired values, so every existing
  asset and gate is byte-unchanged (VK 246/246 + DX12 132/132 re-run to prove it).
- **Gates ×2 backends**: authored FACE CULL through the asset (winding-agnostic dichotomy: exactly one of
  back/front erases the triangle); depth_write=false leaves depth untouched (green-probe NotEqual design);
  a declared bias moves what the depth buffer stores (z=0.5 triangle — at z=0 a float-depth bias is a no-op
  and the gate would pass with the state ignored).
- ⛔⛔ **Found while wiring: blob v3 DROPPED every post-REN-36 pass field** — raygen/miss/closest_hit, VRS
  state, conservative, queue, sampler, filter — and the byte-identity round-trip gate cannot see a field
  dropped by both writer and reader. Blob v4 carries everything; the new gate asserts FIELD SURVIVAL.
- ⛔ **Same class in the vertex emitter**: it wrote the stage NAME and dropped the stage PARAMETERS
  ([tess]/[mesh]/[task]/[cull]/[rt]) — an editor save came back with default levels. Fixed + round-trip gated.
- ⚠ **OPEN (named, not silent): stencil cannot draw a frame yet** — `create_color_depth_target` is D32-only;
  the D24S8/D32S8 attachment path (creation + clears + aspects, both backends) is its own row. Stencil is
  fully declared, cooked, blob-carried, executor-installed and PSO-baked; the attachment is the missing half.

## Gap #6 → the RT stage set: ANY-HIT end-to-end (+ the DX12 pipeline's first real device gate)

- `.crdv` `stage = "any_hit"` with a declared `[rt] alpha_cutoff` (barycentric u+v ignore test — the portable
  OMM fallback; cutoff outside [0,2] refused); emit round-trips it.
- Frame asset: optional `any_hit` on `raytrace.pipeline` (a NAMED one that does not resolve FAILS); executor
  + new appended verb `trace_rays_anyhit`; VK hit group grows the stage (pipeline identity includes it);
  DX12 state object grows a fourth renamed library + `AnyHitShaderImport`.
- ⛔⛔ **Three latent defects found by the gates:**
  1. The HLSL emitter had NO any-hit entry arm — an any-hit fell into the miss branch without the `attr`
     parameter its body reads; DXC refused it (the emitter-lag scar, RT-stage form).
  2. **DX12 `supports_rt_pipeline()` was the A16 Vulkan scar's twin, still live** — it read the lazily-created
     DXR device, answering "no pipeline" on a DXR adapter until the feature had been used. Now answers from
     the feature check. Nothing caught it because **no DX12 device gate had ever run the RT pipeline at all**
     (A16's DX12 half was proven by HLSL-lowering compile only) — the new gate is the first `DispatchRays`
     proof on that backend.
  3. Gates must trace **NON-OPAQUE geometry** (traversal skips any-hit for OPAQUE — the flag's meaning);
     `Dx12RayTracingContext::build_scene_instanced` grew the `opaque=false` option its Vulkan twin had.
- Gate design: cutoff 0 keeps the A16 behaviour (+1/+1/−1/−1); cutoff 2 ignores EVERY candidate → all four
  rays MISS. An any-hit that never reached the pipeline leaves rays 0-1 at +1. Both backends.
- ⚠ **OPEN (named): Intersection and Callable stages** — the IR statement surface (`reportIntersection` /
  `executeCallable`) does not exist in CKIR; building it is a new-vocabulary increment (ops + 4 emitters +
  oracle + SBT surgery both backends), recorded as its own row rather than half-done here.

## Gap #7 → the authored programs RIDE THE ASSET PIPELINE

- **Validating cook handlers** for all five formats (`.frame.toml`/`.crdt` → real binary blobs;
  `.crdm`/`.crdv`/`.crdl` → validated source, binary serializers = named row). Smoke-proven: 4 shipped assets
  cook into a pack; a malformed `.crdm` fails the cook with the cooker's own named error.
- **`builtin_asset_text()`** exposes the embedded pack; **THE DRIFT GATE** parses BOTH sides of every shipped
  asset and compares the canonical emitted form (whitespace-blind, meaning-exact) — ctest-carried
  `CRD_ASSETS_DIR`, SKIP without it.
- ⛔⛔ **The drift gate's first run found `assets/lighting/scene_forward.crdl` was CORRUPT** — every line
  wrapped in quote-comma codegen garbage. It was an inert copy; nothing had ever parsed it. Rewritten from
  the embedded truth; `assets/material/*.crdm` (which did not exist on disk at all) now shipped.
- The parser handles that garbage cleanly in isolation (2 probes incl. BOM-prefixed); a SIGSEGV seen in the
  GATE's failure path when an input is corrupt is a Catch2-reporting-path artifact — loud either way (a
  corrupt asset can never pass), mechanism unpinned, noted honestly.
- ⚠ **OPEN (named): the disk-first mounted load** — the renderer still initializes from the embedded pack;
  loading the cooked authored-program packs through `ResourceManager` is its own row. The drift gate is what
  makes the shipped files REAL (the single source of meaning, enforced) until then.

## Also swept while closing (the uncommitted-work tidy onion, fully peeled)

`bugprone-branch-clone` in `frame_runtime.cpp`; `performance-no-int-to-ptr` (NOLINT + rationale, the
jobs/log precedent) and nested conditionals in `vulkan_raster_context.cpp`; DXR wchar name constants,
`rec_bytes`, blit shader sources, a nested conditional and ~25 function-local `kX` naming violations across
both backend test files; a duplicate include. All tidy targets exit 0 under the LLVM 20.1.8 gate.

## Verification

- kir 261/261 (4 new shape gates) · material-cook 6/6 · vertex-cook 18/18 (+live-contract, +any-hit) ·
  light-cook 12/12 (+garbage probe) · technique-cook 5/5 · frame-cook 16/16 (+state, +blob survival) ·
  scene-render 18/18 (incl. the drift gate + all Vulkan render gates through the live varying contract) ·
  **Vulkan 246/246 · DX12 132/132** after the state plumbing, plus the six new device gates (cull ×2,
  depth-write ×2, bias ×2) and the two RT any-hit gates — all through ctest.
- clang-tidy (win-tidy, LLVM 20.1.8): every touched target exits 0.
- Full `per-slice-check.ps1` sweep: run at session close (result recorded in context.md).
