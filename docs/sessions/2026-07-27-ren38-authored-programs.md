# 2026-07-27 — REN-38 C/D/E/F: THE PROGRAM AS AN AUTHORED ASSET

> **D-007 rows 141 (REN-38 bands C · D · E · F) — 21 rows closed this session (32 → 53 ✅, 12 open).**
> The band's claim: *every* GPU program the renderer runs is cooked from an asset, and the C++ that used to build
> them is DELETED. Three of the four bands were also INTEGRATED into the live renderer and proven on the Vulkan
> render gates; the fourth (F) is proven at the cook layer only — see "What is NOT done" below, which is the
> honest handoff.

---

## What shipped

### Three new cooker modules (the same shape as `crd-frame-cook` / `crd-technique-cook`)

| Module | Asset | Replaces |
|---|---|---|
| `crd-material-cook` | `.crdm` | `MaterialTemplate::build_surface` — a C++ function pointer |
| `crd-vertex-cook` | `.crdv` | `build_scene_vs_shadowed` / `_skinned` / `build_shadow_vs` (~200 lines) |
| `crd-light-cook` | `.crdl` | a `TechniqueBody` C++ builder per lighting technique |

**C band (C1–C3)** — the OpenPBR surface as a node graph. 94 ops, the whole public node library of
`ckir_nodes.hpp`. 14 named cook errors, each provoked by its own malformed asset. ADR-0102 enforced in code
(`shadow`/`light`/`ibl`/`tonemap` refused). The DAG is enforced by DECLARATION ORDER, which makes the cook one
forward pass and a cycle impossible to *write*. Instances resolve to VALUES at cook time, so the constant folder
sees a literal and a layer at weight 0 costs nothing.

**D band (D1–D4)** — the vertex program: header word map, vertex record, instance record, transform and varying
set, all declared. Skinning as a declared SCHEME (N influences, linear-blend **and dual-quaternion**), and MORPH
TARGETS, which had zero code and zero data path. Displacement as a node graph reusing the material registry.
**D4** (a row added by the pre-band audit) is the VS↔FS varying contract, checked by name, location, width and
interpolation.

**E band (E1–E6)** — the lighting vocabulary. `ckir_lighting.hpp` is 1100 lines of gold-standard shading and the
technique ABI carried **exactly one directional light**; none of it was unfinished, there was simply no
vocabulary to name it. Now: light array + per-type counts, all six light types (incl. Heitz LTC rect/tube/disk)
+ IES profiles, SH-L2 + split-sum IBL, decals, froxel clustering, and CSM/spot-map/point-cube × Hard/PCF/PCSS/
EVSM/MSM + contact shadows.

**F band (F1–F5)** — the advanced stages. **CKIR has fourteen stages and the asset reached two.** One declaration
with a `stage` field covers tess control/eval, task, mesh, visbuffer, cull, raygen, closest-hit and miss —
sharing the vertex record, the displacement graph and the varying contract, because a parallel vocabulary per
stage would drift from all three.

### The integration (C4 · D5 · E7) — and the deletions

`scene_renderer.cpp` now cooks its surface from a `.crdm`, all three vertex programs from `.crdv`, and registers
`forward_authored`, whose body is `cook_lighting` over `assets/lighting/scene_forward.crdl`. **The deletion is
the proof**: `scene_build_surface`, the three VS builders and the dead `build_shadow_fs` wrapper are gone.

Also deleted, as the "one system" sweep: `assets/materials/default_lit.mat.toml`, `assets/shaders/*.vert|.frag`,
`tools/asset_cooker/src/cook_handlers/material.cpp` and `.../glsl.cpp` with their registrations — **a second
material vocabulary that actually rendered while `.crdm` merely cooked.**

Shipped assets: `assets/vertex/{scene,scene_skinned,shadow}.crdv`, `assets/lighting/scene_forward.crdl`.

---

## ⛔ SCARS — every one of these RENDERS rather than failing

1. **The skinned VS emitted 2 of 4 varyings.** `build_scene_vs_skinned` wrote locations 0 and 1 and stopped,
   while every cooked fragment program reads 0..3 — so a skinned draw shaded from **undefined interpolants** at
   locations 2 and 3 (the world position the specular and shadow terms need, and the uv). It linked, it bound,
   it rendered, and no validation layer on either backend can see it. Found by 38-D4's contract on its first
   real use. Sharing ONE declared varying set across the three programs is what makes it unreintroducible.

2. **`lighting::pcf_shadow` takes a vec2 uv — a 2-D map — and every atlas here is LAYERED.** Passing a vec3
   lands on `nodes::detail::bin`'s *"two mismatched vectors — a caller error"* arm, which builds a shape-invalid
   node: `cook_lighting` returns a valid node id and the **shader** fails to compile, with nothing pointing at
   the uv width. The array taps must be built explicitly. Same class in the `Hard` path (`tex_sample` where
   `tex_sample_cmp` belonged).

3. **The light-direction convention scar, walked into exactly as documented one function over.** The header
   stores the direction TOWARD the light; a light record's `direction` field is the direction light TRAVELS
   (`directional_light` negates internally). Copying it straight through puts N·L ≤ 0 everywhere — a uniformly
   dark frame that still draws. Negate once, at the boundary.

4. **A hull stage does not pull.** Tess-control runs per CONTROL POINT of an already-assembled patch, so
   `KBuiltin::VertexIndex` is not legal in it and `entry_valid` refuses the graph — correctly. It also must not
   write a position (`stage_writes_position` covers Vertex/TessEval/Geometry/Mesh only), and neither does Task.

5. **`parse_*_toml` never reset its output descriptor.** Parsing a second asset into a reused desc MERGED it with
   the first — surfacing as `DuplicateName` when names overlapped (an error naming the wrong thing) and as a
   silently merged layout when they did not. Present in material-cook AND vertex-cook. **Any tool with a load
   button hits this on the normal path**, not as an edge case.

6. **`VariantKey::vertex` was a reserved field nothing filled** — two layouts hashed to one key, so the cache
   would serve the second the first's program (the dedup collision `ckir_variant.hpp` names for an undeclared
   axis, arriving through a declared one). `vertex_layout_id` is the value; see the caveat below.

7. **Two registry defects only a coverage gate could find.** `kMaxNodeInputs` was 5 while `gooch_shade` and
   `range` take SEVEN, making those two unauthorable and rejected for a reason that named the wrong thing; and
   **not every argument is a wire** — `extract`'s index, `convert_f_vec`'s width, `place2d`'s order, the
   geometric readers' `location` are compile-time attributes that are *also* spelled `int`, so a node id lands
   in them, type-checks, and swizzles component 47.

8. **TOML table scoping, twice.** A bare key written after `[[varying]]` belongs to THAT table, not the root — so
   a `[tess]` section placed ahead of `schema` swallows it and the asset reports `BadSchema`. Root keys before
   the first table; tables after.

---

## ⛔ GATES I HAD TO STRENGTHEN (a check that cannot fail is not a check)

- **C3**: my first probe values (0.25, 0.8) also occur in the base graph and in `surface_defaults`, so those
  checks would have passed with the override never reaching the cook. Every probe is now unique to one instance,
  and the gate asserts the replaced default is GONE, not merely joined.
- **D3**: the displacement gate first only checked the literal was present — which passes with the node built and
  never wired. Both graphs now run through B7 `lower_entry`, so the constant survives only if the clip position
  genuinely depends on it, with an unwired control that must be DCE'd away.
- **E2 (IES)**: the baseline used the shared record, which already declares `ies_index` — it compared the feature
  with itself and passed with the profile lookup absent.
- **E5**: clustering's claim is that it BOUNDS the unrolled loop, so the gate asserts 16 point lights culled to 4
  per froxel cook a **smaller** program than 16 walked directly. "Did it cook" would pass with the cluster list
  declared and ignored — and the only symptom would be a frame time nobody expected.

---

## ⚠ WHAT IS **NOT** DONE — the honest handoff

> **SUPERSEDED (same day, parts 2–3):** all four items below are CLOSED — F6 renders on both backends,
> `ckir_draw.hpp` is deleted, `VariantKey::vertex` is engine-filled, and the full sweep ran at session
> close. Kept verbatim as the record of where part 1 stopped; see Part 3 for the closures.

- **38-F6 — the advanced stages are not wired to the renderer.** F1–F5 are proven at the COOK layer only. No
  `.crdv`-driven mesh, tessellation, RT, cull or visbuffer pass draws a frame; `SceneRenderer` still creates
  exactly three program pairs, all `stage = vertex`. This is the same integration gap the post-E audit found, one
  band later. Close it the way C4/D5/E7 were closed — a frame asset naming the pass, the renderer cooking the
  declared stage, and a Vulkan gate that RENDERS. **Both scars 2 and 3 above only appeared once something
  actually rendered**, which is the argument for doing this before the G band.
- **38-F7 — `engine/draw/ckir_draw.hpp` is still a C++ builder** (339 lines: line/tri/grid VS+FS). It IS CKIR, so
  not the GLSL-class redundancy that was deleted, but the same class as `scene_build_surface` was. Not a
  mechanical port: the line VS is a screen-space quad expansion with a 6-corner select chain over a per-instance
  LINE record.
- **`VariantKey::vertex` is still not filled by engine code**, and the D5 row originally claimed it was — now
  corrected in place. No engine code cooks a variant matrix at all (the only `VariantRequest` constructors are
  tests), so there is no call site. `vertex_layout_id` is gated as the correct value for the axis.
- **The full `per-slice-check.ps1` sweep has NOT been run** — everything above is targeted module runs plus the
  Vulkan render gates, at the user's explicit direction ("I don't want to lose time with full slice").

---

## Verification

- `crd-material-cook-tests` 6/6 (576 assertions) · `crd-vertex-cook-tests` 16/16 (273) ·
  `crd-light-cook-tests` 11/11 (139) — all through **ctest**, not binary-direct.
- Vulkan render gates: `GEO-7 GATE` (10k instances) · `REN-2 Half B GATE` (samples a base-color map — i.e. the
  textured `.crdm` with `sample2d`) · both `REN-3.2-b GATE` shadow cases · `REN-37.2 GATE` (technique swap,
  now including `forward_authored`). 124/124 scene-render CPU gates.
- clang-tidy (LLVM 20.1.8 gate) clean on every touched file; two pre-existing naming violations in
  `test_scene_render_gpu.cpp` fixed rather than left dirty.
- All new files CRLF.

## Part 2 (same day): 38-F6 + 38-F7 — the renderer joins, and the last C++ builders die

### 38-F6 — the renderer draws through the advanced stages

The join immediately falsified the F band's cook-only closes. **Five device-impossible cooks** had shipped green:

1. **TessEval pulled by `VertexIndex`** — legal only in a vertex stage, over a tese emitter that lowers no
   storage. Rewritten: position = the emitter's bilerped `TessPatchPosition`, optionally DISPLACED by the node
   graph; varyings are `node:`/`clip.w` terms only (attribute varyings are REFUSED, not dropped).
2. **Mesh pulled the same way** — rewritten as the procedural meshlet grid (thread tid writes corner tid%3 of
   triangle tid/3), which forces `max_vertices == 3 · max_primitives` (now validated; the old DEFAULT 64/124
   violated it — primitive 124 indexed vertex 374 of a 64-vertex budget).
3. **Task rode the vertex pull tail** — `entry_valid` refuses a task entry with a position or outputs. Now an
   early amplification-only arm.
4. **The cull kernel could never emit** — it read through the raster `StorageLoad` seam and `GlobalInvocationId`,
   neither of which the compute kernel emitter lowers, and declared its args buffer at binding 3, which no
   frame pass can bind (reads+writes bind in declaration order from 0). Rewritten on `BufferLoad` +
   `WorkgroupIndex·ls + LocalInvocationIndex`, args at binding 1.
5. Every F-band gate now asserts `entry_valid` on the cooked entry — the missing check that let all of this close.

Renderer seams: `set_frame_graph_toml` (explicit graph OVERRIDES the shadows-tier step-down — that tier is the
forward pair's capability contract, not the frame's), lazy `ensure_*` program cache in the host
(`crd://scene/tess|mesh|visbuffer` + `crd://scene/cull` + `crd://scene/rt/*`), `set_scene_accel`,
`debug_scene_buffer`. New builtin+shipped declarations (11 `.crdv`, `material/flat.crdm`, 5 `.frame.toml`), all
drift-gated. Three Vulkan gates RENDER: tess (displacement-territory pixels), mesh+task, visbuffer (two
primitive-id greys), cull (real frustum verdicts read back), RT (4 rays through the 4-stage authored pipeline
over a non-opaque TLAS).

### 38-F7 — the debug-draw suite is authored; `ckir_draw.hpp` is DELETED

The vocabulary that made it possible (in `.crdv`): `position = "node:<name>"` (node-computed clip — a
PROCEDURAL vertex stage with no pull), `[expand]` (verts_per_instance, instance record, the `@category`
scheme), input spellings `@corner` / `@instance` / `field:` / `fieldu:` / `fieldc:` / `hdr:` / `hdru:` /
`hdrc:`, and the vertex-cook-local `view_proj` op. The material registry gained `fwidth` (fragment-only, for
the grid's cell factor). line_aa (screen-space quad expansion, ~80 nodes), triangle_solid and the infinite
grid are now `.crdv` + `.crdm` pairs — embedded in `draw_assets.hpp`, shipped under `assets/`, drift-gated —
and `crd-draw` init cooks them (vertcook + matcook through `unlit`). THE DELETION IS THE PROOF: 339 lines of
hand-written CKIR gone, and the RET-6 Vulkan pixel gate still composites the line over the scene.

Suites at close of part 2: draw 17/17 · vertex-cook 19/19 · scene-render 21/21 (3 new F6 device gates) ·
RET-6 overlay pixel gate green. Full sweep pending at session close.

---

## Part 3 — the F-band closes end to end (F8–F16), and the DX12 gates come back from the dead

The continuation directive was "fully finish 38-F, no gaps no deferrals". The audit rows (F8/F9/F10/F12/F14)
had landed earlier; this part closed F11, F13, F15, the GPU-driven chain, and the two geometry-pull rows that
became 38-F16 — then the close itself found the biggest defect of the day.

### 38-F11 — the stencil ATTACHMENT

`create_color_depth_stencil_target` (vtable END, both backends). VK: D24S8 with both aspect bits, 13 layout
sites + 13 attachment hookups (including the frame-path `record_*` ternary spellings the first regex pass
missed — the gate reported "stencil passes everywhere" and that is what found them), combined-aspect barriers.
DX12: the PSO `DSVFormat` keys on the TARGET (8 sites), clears carry `CLEAR_FLAG_STENCIL` (11 sites). The
close forced the pass-level `load = true` key into the frame vocabulary (blob v5, `LoadNeedsGeometry`
validation): two raster passes on one target used to re-clear colour+depth+stencil, so mask-then-test — the
thing stencil exists for — was un-authorable. Gates on both backends run the mask/cover dichotomy.
TOML compare names are CamelCase ("Always", "Equal") — the parse error names the row.

### 38-F13 — Intersection + Callable, the last two CKIR stages

CKIR grew `CallableDataDecl` + `ReportHit`/`ExecuteCallable` (payload load/store REUSED over the callable
block); both RT emitters accept the stages, lower report/execute + the object-ray builtins + `Sqrt`, and —
found here — gained If-body NESTING (the flat statement loop silently dropped If bodies). `.crdv` authors an
analytic-sphere intersection (BOTH quadratic roots — an inside-origin ray needs the far one) and a callable
transform; frame graphs name both on `raytrace.pipeline` (blob v6). New verb `trace_rays_full`; VK packs a
PROCEDURAL hit group + a 4th SBT region, DX12 a PROCEDURAL_PRIMITIVE group + the CallableShaderTable. Gates on
both backends pin the pair by NUMBERS: hits 3.0 with the callable, 1.0 without. Closing F13 also pinned the
audit's unpinned SIGSEGV (F15's throwing-toml++ finding below).

### 38-F15 — disk-first mounted load, and the crash it pinned

`set_asset_root(dir)`: a file under the root SHADOWS the embedded pack for every authored asset; a disk copy
that fails to parse REFUSES by name with zero passes executed. The gate's first honest form found the audit's
SIGSEGV: three cookers still parsed with THROWING toml++ while three parsed with `TOML_EXCEPTIONS=0` (an ODR
hazard on top), and a thrown `parse_error` unwinding under a live device's validation layer killed the
process. All five cookers now parse non-throwing. The gate asserts EXECUTION truth (`timed_passes`), not
pixels — readback memory of a destroyed target recycles.

### The GPU-driven chain + VariantKey::vertex

The cull kernel now writes REAL indirect args ({survivors,1,1} via thread-0 reset + buffer barrier + atomic
adds, guarded by the new header word 100 = instance count; header 116→120 words) and an authored
`compute.indirect` mark pass proves `marked == visible` on both backends. `VariantKey::vertex` is filled from
the live `.crdv`'s folded `vertex_layout_id` at `init_programs`.

### 38-F16 — amplification pulls REAL geometry (the epilogue the band demanded)

The F6 rewrite had made tess/mesh/task procedural-only because their emitters lowered no storage reads. Now:
- task/mesh/tese emitters (GLSL + HLSL) declare the `sbuf` seam when a `StorageLoad` is reachable; the VK
  set layout names TESC/TESE (+TASK/MESH when present).
- Verbs at vtable END on both backends: `draw_tess_storage(_load)`, `draw_mesh_storage(_load)`; the executor
  binds a draw item's storage for both amplification kinds.
- Vocabulary: `[mesh] fetch = true` (the SAME pull contract as the scene VS — indices → records → visible
  slot → instance matrix → view_proj; morph/skin refused), `[task] emit_header = N` (the dispatch count from
  a buffer word — GPU-driven meshlets), `hdr:`/`hdru:` inputs in tess_eval + fetch-mesh graphs.
- ⛔⛔ THE AS→MS PAYLOAD CONTRACT: HLSL `DispatchMesh` always passes a payload; D3D12 rejects a pair whose
  payload sizes disagree; Vulkan tolerates absence on both sides. `[mesh] payload = true` declares the
  pairing (`KEntry::mesh_payload_in`, appended + serialized — graph blob v3).
- Gates ×2 backends: tess pull + a `hdr:22`-scaled DOMAIN (the tese lowering proven by pixels), mesh fetch
  (extent tracks the buffer), emit_header (word 130 decides how many quads exist), plus the cook probe.

### ⛔⛔ THE CLOSE'S HEADLINE: every DX12 scene render gate had been silently dead

The F16 suites showed the three DX12 F6 scene gates SKIPPING as "dxc/DXIL unavailable". The truth: the
authored-technique FS reads locations 2,1,0 in node order, the HLSL emitter declared `PSIn` in that order, and
DXIL links inter-stage varyings by PACKED REGISTER — every scene graphics PSO on DX12 failed link with
`E_INVALIDARG`, and the guard translated a null program into a SKIP that ctest counts as "passed". This is
the pixel-side twin of the SV_Position-LAST scar. Fixes: `PSIn`/`VSIn` StageIns now declare SORTED BY
LOCATION; the visbuffer gate probes moved to the horizontal midline (D3D mirrors the fullscreen quad's
diagonal vs Vulkan, so the old corner probes both landed on one triangle). All three gates now RUN and PASS.
Lesson recorded: audit the SKIP list, not just the failure count.

Tidy's switch-completeness also caught a REAL F13 emit hole: Intersection/Callable fell out of the `[rt]`
emit arm, so their canonical form dropped `sphere_radius`/`callable_*` — the exact fields those stages exist
for. Fixed + round-trip gated.

Suites at part 3 close: the touched families (REN-3*, CKIR, v17, vertex-cook, frame*) run 453/453 with ZERO
skips in the REN-38 set; kir serialize 5/5 at graph-blob v3. Full sweep at session close.

### The Linux epilogue (user-directed: "win-debug + linux release, CI owns the rest")

The Windows sweep was cut after win-debug (full suite green after the typed-units guard fix — five justified
`crd-lint-allow-untagged-physical` suppressions: NDC/device/object-space scalars, not physical quantities).
The first WSL `linux-gcc-release` run in months then earned its keep, in order:

- **gcc `-Wswitch` found THREE more emitters missing the F13 statement arms** (CUDA, MSL, WGSL + the CPU
  kernel oracle) — tidy had only covered the files I touched; the wire-ALL-backends scar, fifth occurrence.
- **The frontier NV extensions don't exist in distro Vulkan headers** (1.3.27x vs the cluster-AS/LSS types).
  Fixed structurally: `FetchContent` pins Khronos Vulkan-Headers to the SAME SDK tag Windows CI caches
  (`vulkan-sdk-1.4.341.0`), `BEFORE PUBLIC`, populate-only. One code path; also un-reddens Linux CI.
- **My DX12 twin gates leaked into the Linux build** — now `#ifdef _WIN32` + a conditional CMake link.
- **Committed-code portability nits MSVC never flags**: unused function (frame_compose), `-Wshadow` ×2
  (lighting_asset, hair-scatter test), `-Wdouble-promotion` (timeline, audio, sandbox, audio test),
  MSVC-only `strtok_s` in the ceridc MCP tool (portable `CRD_STRTOK` now).
- **The no-malloc guard flagged six committed test fixtures** (`MallocAllocator` globals in ceridc + five
  cooker suites) → named `GrowableTlsfAllocator`s, green on both OSes.
- **The HLSL-validation family failed without dxc** — `compile_hlsl_to_spirv`'s contract says the
  `CRD_HAS_DXC=0` stub exists "so conformance tests soft-skip", but `test_ckir_glsl_compile.cpp` never wired
  it. Now keyed on the stub's unique marker; every real dxc rejection still fails.

**⛔ OPEN, recorded not masked:** WSL runs MORE than CI — llvmpipe is a real (software) Vulkan device, while
CI installs the loader with no ICD, so every GPU-dispatch test SKIPS on CI. Under llvmpipe the B19 gsplat
suites SIGSEGV/timeout (a shader OOB corrupts HOST memory there — possibly a real OOB that GPU robustness
absorbs), the B-cmp radix sort aborts, and the B11/B16/B18 to-ULP oracles mismatch (llvmpipe transcendentals
are libm, not the HW the claims were calibrated on). Triage owed the day a Linux GPU claim matters — the
segfault family first. `reference_wsl_linux_sweep_and_llvmpipe_exposure.md` carries the map, including the
CI-mirror trick (`VK_DRIVER_FILES=/nonexistent ctest`). **FINAL VERDICT (native WSL, llvmpipe present):
5,203 / 5,220 green — all 17 failures in the recorded llvmpipe-only dispatch class** (B19 ×5 · sort · subgroup
×2 · hair/fur/transcendental ULP ×5 · D2 cook-run · AS autotuners ×2 · v9e-b), none of which CI executes.
Windows: win-debug FULL suite green (5,482, one comment-only guard fix); asan/shipping/tidy ceded to CI at
the user's direction.

---

## Part 4 — "FIX THIS FULLY": the llvmpipe 17, triaged to four root causes, all killed

The user rejected the recorded-open handoff for the llvmpipe failures. The campaign triaged all 17 into four
classes and fixed every one — and the biggest finds were REAL kernel defects the whole NVIDIA-only history had
been absorbing:

### Class A — the subgroup-width assumption (the SEGFAULT family, 8 tests)

llvmpipe's subgroup width is 8, fixed (min=max — not even pinnable to 32). Three genuine defects fell out:

1. ⛔⛔ **Phantom-lane ballot complement** (`ckir_sort` scatter + onesweep): the digit-match's `~ballot` arm
   sets every bit above the device's subgroup — `BitCount` then hands the leader up to 24 ghost lanes, the
   per-digit counts explode, and the staged ranks run past the shared arrays. Reproduced DETERMINISTICALLY on
   the CPU oracle once the oracle was taught the device width. Fix: the match starts from the ACTIVE-lane mask.
2. ⛔⛔ **Unguarded tail threads** (2DGS project): dispatches round up; threads past the surfel count read and
   write OOB. `Gsplat2dProjectConfig::count` + an If-guard over the whole body.
3. ⛔⛔ **Eager-`Select` sentinel load** (StopThePop resort): `Select` evaluates BOTH arms on the GPU exactly
   like the scalar oracle, so `hit(best_idx)` at the not-found sentinel loads one-past-the-end. Fix: clamp the
   index BEFORE the load; the clamped record stays select-discarded.

Systemically: `eval_cpu_kernel` **now asserts on OOB** (it used to imitate robustBufferAccess with silent 0.0 —
which is exactly how 2 and 3 passed every oracle gate; the assert immediately caught 3);
`IComputeContext::subgroup_size()`/`shared_memory_bytes()` (vtable END, VK+DX12 queries); the sort builders take
`lanes` EXPLICITLY and `pick_sort_config` derives the device-true shape (llvmpipe: 16-thread/4-bit/8-pass);
the sort oracle test now runs BOTH shapes permanently; the shared subgroup kernel's shuffle uses
`(tid+1)&(lanes-1)`; ballot-test oracles group by the device width. DX12 face: WARP reports wave 4 — the same
adaptation covers it.

### Class B — NVIDIA-calibrated tolerances on native ops (6 tests)

The native-op tier tests asserted NVIDIA's delivered precision, not the Vulkan spec's guarantees (inverse-trig
is granted 4096 ULP ≈ 5e-4 relative; llvmpipe uses the headroom, conformantly). Tolerances are now
SPEC-DERIVED conformance envelopes with the derivation and both measurements (NV + llvmpipe) in the comments —
the exp-amplified BCSDF cones get the condition-number bound (|v|≈40 amplifies a conformant argument error
~40×). The bit-exact claims live untouched in the deterministic tier.

### Class C — dxc-absence asserts (D2)

The offline-cook test demanded DXIL bytes unconditionally; a trivial-HLSL availability probe now gates the
DXIL halves (the same documented soft-skip contract as the conformance suite).

### Class D — environment-stale autotune rows (both autotuners)

A tuned row is a measurement cache, and the SAME sm_89 silicon under WSL-CUDA measured the Windows-tuned
attention tile 2.27× slower than that run's own winner. `TuningEntry`/`AttentionTuningEntry` now carry the
OS they were measured on (`tuning_env()`), the generator emits it, and rows replay ONLY in their environment —
everything else falls back to the heuristic. AS-6b's 200-GFLOPS floor keys on
`VkPhysicalDeviceProperties::deviceType`: a CPU device owes correctness, not throughput.

Windows re-verification at every step stayed green (the 32-lane shape is what the pickers derive on NV/DX12,
bit-for-bit the historical behavior). Final: full suites both platforms — numbers below.

### The layer un-hid 8 more scene gates (they had SKIPPED on Linux forever)

Installing the validation layer to debug B19 also un-hid 8 scene-render gates that REQUIRE validation and had
silently skipped in every prior Linux run. Four now pass (after two MORE real fixes the layer forced: the
engine had never ENABLED `taskShader` while creating TASK shader objects — VUID-08421 on every platform, NV
just tolerated it — and with the feature enabled, the shader-object completeness rule requires TASK bound as
VK_NULL_HANDLE on every non-task draw; both fixed, plus a default blend-equation baseline for old-layer 09418
false positives). ⛔ OPEN (llvmpipe-only, 4 gates): REN-3.2-b slanted / REN-37.2 technique swap / REN-37.8 +
REN-37.10 viewports return an ALL-ZERO readback (not even the clear alpha) with draws recorded and validation
clean — a submission/readback interaction unique to those harnesses on llvmpipe; their siblings on the same
machinery render fine. Evidence pinned here; Windows green throughout.

### Where part 4 STOPS (honest state)

The OOB assert added in Class A is doing its job beyond the tests it was written for: the Windows full suite
now fails 9 kernel tests with `eval_cpu_kernel: OOB buffer READ - guard the tail threads` — the 3DGS family
(project/render/tiled/mip-splatting, B19-a4 tilecount, shared-block render, the 2DGS→TSDF chain) and the
inline-rayQuery oracle. Same class, same fix shape as the 2DGS project (declared `count` + If-guard, or a
pre-load clamp for Select sentinels) — REAL OOB reads that used to be silent. ⛔ The assert must NOT be
weakened to make them green. Also open: the 4 llvmpipe scene gates, whose cause is now pinned to a draw-list
binding with a NULL program (the recorder now REJECTS that loudly instead of skipping silently — a real
silent-black-frame hole closed); what remains is why the host resolves no program on a capability-reduced
device. `crd-simd-emission-check` in a bare `ctest` is the known vcvars/dumpbin scar, not a regression.

### The llvmpipe scene gates were a NAME-MANGLING bug, not a graphics bug

⛔⛔ `World::component_id_by_name` matched an authored component name against `typeid(T).name()` using only the
MSVC decoration ("struct crd::scene::MeshRenderer" — the string ENDS with the identifier). The Itanium ABI gcc
and clang use produces "N3crd5scene12MeshRendererE": LENGTH-PREFIXED components, terminated by `E`. A trailing
match therefore NEVER succeeded on Linux/macOS, `component_id_by_name` returned null, `group_matches` rejected
every group, the authored draw list resolved EMPTY, and the renderer drew nothing — silently, on every gcc
build, since REN-36.3-b shipped. `decorated_names` now tries BOTH decorations, gated by a test that asserts the
two literal spellings so it fails on whichever compiler the author is not using. All 8 scene gates green.

⭐ The lesson is diagnostic, not technical: this presented for hours as "SceneRenderer frames are black on
llvmpipe while gpu-context frames render fine", and I chased barriers, layouts, imported targets and readback.
What broke it open was making the silent skip LOUD (the recorder now names a program-less drawing pass) and then
noticing that `group_matches` was called on Linux and NOT on Windows — a platform split in pure CPU logic, which
no graphics explanation can produce. **When a "GPU bug" splits by COMPILER rather than by DEVICE, stop looking
at the GPU.**

### The renderer-weakness campaign (same day): shadows+albedo COMPOSE, the heap, DrawIndex, MULTI-DRAW

Four slices, user-directed ("close those gaps, best full frontier implementations"):
1. **Torus unlit — CLOSED.** The orientation fix existed; the missing piece was the ARTIFACT-CLASS gate: a
   genus-1 CW/CCW torus through weld+generate_normals, outwardness by the RING-DISTANCE metric (a torus is not
   star-shaped — the centroid metric cannot test it). The shipped torus.obj measured 6V = -18.35 (CW).
2. **Shadows/albedo exclusivity — DEAD.** The atlas moved to its OWN bindings (VK 4/5, DX12 t4/s5); the
   renderer cooks a COMBINED textured+shadowed variant; the executor routes item-texture + pass-depth-read to
   the new combined verb. Gate: the CONJUNCTION (texture visible + shadow lands + texture survives shadows-on).
3. **The bindless heap is real:** kBindlessMax 8 → 1024 both backends, PARTIALLY_BOUND enabled when offered,
   fill sites write only the registered slots.
4. **MULTI-DRAW:** `KBuiltin::DrawIndex` (GLSL `pc_draw.index + gl_DrawID`, HLSL root-constant b7 — one shader
   serves single AND batched draws); `draw_storage_multi_depth` = ONE vkCmdDrawIndirect / ONE ExecuteIndirect
   over an args ring; the executor coalesces consecutive plain items; the gate asserts BIT-IDENTICAL pixels
   AND `multi_batch_count()` delta == 1 (pixels alone cannot distinguish batched from looped).

⭐⭐ **AND THE SCENE-BUFFER CONSOLIDATION LANDED (user-directed, same session):** one renderer-owned buffer —
frame header at words [0..119] (the FS reads absolutely, so it needed NO change and no flat varying), the draw
table at [120..375], group regions from 384 as exact images of the private layouts. The scene VS is the SAME
declaration plus one `rebase_table = 120` line; `Vx::loadu` (the single load choke point) rebases everything.
Plain groups on a shadow-free single-viewport frame now render as ONE multi-draw batch — gated by TWO DISTINCT
mesh groups producing `multi_batch_count()` delta == exactly 1 with both halves' pixels present. Scars: (a)
`gl_DrawID` must be spelled `gl_DrawIDARB` under #version 450 (shaderc fails silently and the fallback keeps
pixels right while batches read 0 — the COUNT gate caught what pixels could not); (b) `shaderDrawParameters`
must be ENABLED at device creation; (c) an explicitly installed frame graph (the authored CULL graph) computes
visibility into the PRIVATE buffers, so consolidation defers to it — found by the CULL gate, not inspection.
