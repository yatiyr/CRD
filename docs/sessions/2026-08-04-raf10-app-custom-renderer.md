# Session — RAF-10: an application customises the renderer ten ways, no engine edits (2026-08-04)

**Branch:** stacked on the RAF-9 tail (which sits on the RAF-8 + A13-VRS-fix WIP).
**Gate 10:** a small app package customises the renderer in every intended way, on BOTH backends, WITHOUT editing a
line of engine rendering code — and using NO privileged engine-only path (no engine-private method, no backend
virtual, no central-enum edit, no hard-coded backend slot, no embedded frame string, no bypassed validation).

## What shipped

RAF-9 wired *identity* into content resolution (the default renderer loads by `engine://` id through a public
registry). RAF-10 proves the OTHER half: an **application** drives that same public surface to customise everything,
and the proof is a public-headers-only test package that renders all ten customisations bit-for-real on Vulkan + DX12.

### The public engine seams (no engine-private path)

- **App asset mount** — `set_app_asset_root(dir)` mounts `app://` alongside `engine://` (RAF-1 scheme, RAF-9 resolver).
- **App material** — `set_scene_material(opaque_id, textured_id)` overrides the two hard-coded `material/scene*.crdm`
  names with canonical ids; the single cook site (`cook_fs`) now routes through a new `resolve_asset_text` that reads a
  `://`-schemed id through the mount table (so an `app://material/…` resolves) and a bare name through the engine mount
  (byte-identical to before).
- **App technique** — `define_technique(const kir::technique::Technique&)` registers an app technique through the SAME
  `TechniqueLibrary::define` the engine uses for its own `scene_authored_technique`; replayed AFTER the builtins in
  `init_programs`, so a same-named app technique shadows (find is last-match). ⛔ Not a weaker path — it is the engine's
  own C++-body mechanism. (A fully `.crdt`-asset-driven technique needs a `.crdt`→CKIR body serializer, explicitly out
  of scope per RAF9plan.md:162 — flagged, not silently skipped.)
- **App display transform** — `register_post_asset(id, crdp_name)` registers a fullscreen program provider that cooks an
  app `.crdp` the SAME way the engine cooks its tonemap/sRGB (`ensure_post_program` generalised to
  `ensure_post_program_named`).
- **App custom pass EXECUTOR** — `register_pass_executor(id, fn)` joins an app `PassRecordFn` into the recorder's ONE
  `GraphExecutorTable`. A `kind = "custom"` frame pass names the id in `executor = "app://executor/…"`; the new
  `FramePassKind::Custom` + `FramePassDesc::executor` field + `record_custom_via_executor` adapter resolve it in the
  same table a builtin uses and drive it with the resolved payload. The id is the extension point — a new pass MECHANIC
  with no `FramePassKind` edit and no backend virtual.
- **Capability query** — public `capability(name)` forwards to the ONE predicate (`Impl::capability`) the frame-graph
  `requires`/`fallback` step-down consults, so the answer an app inspects equals the answer the recorder acts on.

### The app package (Gate-10 proof)

`tests/scene-render/test_raf10_app.cpp` + `tests/scene-render/app_assets/` — public headers ONLY. It does all ten:
(1) engine `forward_basic` unchanged; (2) `app_custom` INCLUDES `forward_basic` as a subgraph; (3) injects `app_grade`
at a declared anchor; (4) an app `.crdp` AgX display transform; (5) `app_scene.crdm`; (6) the `app_tint` technique
(reuses the engine's proven `directional_light` + a warm tint, built with the public CKIR API); (7) a fully
app-authored `app_authored` graph; (8) a custom C++ executor doing the fullscreen grade; (9) `app_gated` requires an
unmodelled capability → deterministic step-down to `app_basic` (pixel-identical to `app_basic` rendered directly);
(10) the whole thing again on DX12. **61 assertions, 2 cases, all green.**

## Three real engine bugs RAF-10 surfaced (composition + app-registration were never actually rendered before)

1. **`register_default_programs` idempotency guard** keyed on `raster_count() > 0` — an app pre-registering ANY program
   (e.g. its post asset) before `init_programs` made the guard skip registering EVERY engine default → the whole scene
   went black. Fixed: a dedicated `default_programs_registered` flag.
2. **`copy_pass_body` (frame_compose) was incomplete** — it predated the REN-38/40 pass fields and silently DROPPED
   `executor` (so an injected custom pass validated as "a fullscreen pass with no shader") plus ~18 others (RT shaders,
   blend, render state, load/depth attributes, sampler). Fixed: copy every field; `shared_depth` (a resource name) is
   namespaced in the caller. A comment now pins the invariant.
3. **The composed graph was installed WITHOUT re-validation** — `set_frame_graph_toml` now runs
   `validate_frame_graph` on the flattened result (composition is not a weaker path than hand-authoring).

## Verification (Gate 10 met)

- **RAF-10 GATE (Vulkan) + RAF-10 GATE (DX12)** — 61 assertions, both green: engine default renders; app-authored
  differs from it; composed+graded renders and differs from app-authored; the gated frame is pixel-identical to its
  fallback; `capability()` mirrors the real device.
- frame-cook device-free suite **459/459**. Full scene-render suite: **58/62** — the 4 failures (REN-39-C1 pull/indexed,
  REN-40-D moment) are the SAME pre-existing RAF-8-baseline failures the RAF-9 log documents (unchanged by RAF-10; the
  38-G1 engine post tests are untouched — the `ensure_post_program` refactor is byte-behaviour-preserving).

## Flagged (NOT RAF-10 scope, separate slices)

- **`pbr_neutral` / `saturate` post ops fail `create_program`** (CKIR→SPIR-V lowering) — `cook_post_graph` succeeds but
  the backend refuses the program. Pre-existing latent gap (these ops sit in the op table but were never exercised
  through a real post cook→build); `agx`/`srgb_encode` build fine, so the app grade uses AgX. A material-cook/CKIR
  investigation, not a RAF-10 seam defect.
- Fully `.crdt`-asset-driven techniques (the `.crdt`→CKIR body serializer) remain a future technique-cook slice.
- The 2 pre-existing RAF-8-baseline GPU-gate failures (REN-39-C1, REN-40-D) still await their own investigation.

## Next

RAF-11 (hot reload — the `content_hash`/reload-safety property RAF-3 built for) → RAF-12 (DELETE the legacy draw verbs
+ `FramePassKind` switch, the deletion-is-the-proof close) → RAF-13 (docs + §22 DoD).
