# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

> Closed + superseded entries were pruned 2026-08-07 per this file's own rule; their dispositions (and the
> full text of every entry without a session-log home) are preserved in
> `docs/sessions/2026-08-07-doc-hygiene-pass.md` (appendix). Older versions: git history.

## Active debt

### `b16-b4-ocean-mesh-followons` — B16/B4 open follow-ons after the displaced-ocean + mesh-shader session (filed 2026-07-16)

> Not blockers — the ocean renders (Vulkan) + the mesh path is proven; these finish B16/B4 in gold standard. Session:
> `docs/sessions/2026-07-16-b16-displaced-ocean-mesh-shaders.md`. Mesh device scars: [[feedback_mesh_shader_device_scars]].
>
> 1. ~~**B16-close DoD**~~ — ✅ DONE: B16 closed 2026-07-18 with the full ctest+shipping sweep (see the SANITY ledger
>    2026-07-18 entry — the close peeled a five-layer onion the binaries had hidden).
> 2. ~~**DX12 device mesh render**~~ — ✅ core LANDED since: the DX12 raster context ships `DispatchMesh`/mesh draws
>    (see [[feedback_mesh_shader_device_scars]] + [[feedback_draw_mesh_storage_had_no_synchronous_path_both_backends]]);
>    the visibility buffer landed (REN-38-F6, now RAH-1a.1 typed attachment). Amplification landed via F16. Remaining
>    tessellation coverage is tracked post-RAF (GVA/RPL bands), not here.
> 3. **WGSL/MSL portability of the cascade sampling** — `SampleIndexedLod` is bindless (descriptor-array); WebGPU has no bindless,
>    so the portable form is a `texture_2d_array` layer + `textureSampleLevel`. Rework the ocean cascade textures to a layered
>    array so the vertex-pull ocean lowers to WGSL/MSL too (mesh shaders don't exist on WebGPU — that stays vertex-pull anyway).
> 4. **Minor ocean visual polish** — a faint residual horizon line, a slightly softer sun, more directional god-ray shafts.
>    → [[project_ocean_visual_gaps_before_b16_close]].

### Future cluster — direct-manipulation UX (gizmos / mesh + curve + navmesh editors) — filed 2026-05-19

> **Not a follow-on; a future workstream cluster.** User flagged (during
> Phase 3.1.7 v10e control-point editing question) that direct-
> manipulation UX is a high-priority future area requiring its own
> session cluster. Slated for the UI phase, possibly preceded by a low-
> level plumbing detour. Until then, sandbox showcase scenes use ImGui
> DragFloat3 / sliders for control-point + parameter editing.
>
> **Scope (user's framing):**
>
> - **Transform gizmos** — translate / rotate / scale handles on selected
>   entities, screen-space picker + camera-relative axis lock.
> - **Curve control-point gizmos** — drag curve control points in the 3D
>   viewport. v10e's ImGui DragFloat3 panel is the temporary surface
>   the gizmo cluster eventually replaces.
> - **NavMesh editing** — vertex / edge / face manipulation.
> - **Mesh editing — Blender-class** — vertex / edge / face select +
>   deselect + drag (equal-class consumer alongside game + cinematic).
> - **Selection state primitives** — multi-select, select / deselect,
>   marquee, hover-highlight.
>
> **Likely module surface (placeholder, not committed):** `crd-ui-gizmo`
> (ray-vs-handle hit-testing + drag-state machine + axis-locked
> translation) consumed by sandbox + future editor.
>
> **Architecture written up 2026-05-22 → `docs/phases/phase-ui-tooling.md`**
> ("Cerid UI & Tooling Architecture — `crd-ui` · gizmos · editor overlays").
> Elite design captured: **two worlds** (document vs transient tooling world —
> gizmos/editor-UI are `EditorOnly`-tagged entities, hidden from the scene-tree
> panel, not saved, not in the user's undo stack); the **Logic / Visual /
> Command triple** (gizmo = a System + a swappable Visual [entities OR
> `crd-geometry-viz` immediate-draw] + a committed command-verb); gizmos and UI
> are the same pattern (entities + systems + command-verbs); lifecycle
> (selection→spawn→drag→commit-one-command-on-release); hit-test reuses
> `crd-geometry-spatial`; elite traps (constant screen-size, overlay-pass depth,
> pointer-capture, selection-as-shared-state, gizmo-never-the-only-path). UI
> rendering = screen-space 2D frame-graph pass vs worldspace renderable;
> shader/resource changes ADDITIVE not structural; `crd-font` (MSDF) is the real
> new substrate. Sequences after renderer + `crd-font` + `crd-scene` + the
> command layer (`docs/phases/phase-4.0-platform.md`).
>
> **Sequencing:** undecided. Slots in EITHER after `crd-hesap-dense` v0
> + Phase 3.1 eylem v1c-resume (consumer-pull from eylem's
> collider/joint editors) OR before, depending on when editor UX
> becomes the critical path. Discussed at the close of Phase 3.1.7.
>
> **Until then:** all geometry + curve + future mesh-viz sandbox scenes
> use ImGui DragFloat3 / sliders — do NOT bake a half-built ad-hoc
> picker into individual scenes.

### `v2b-amd-cs_amd-tiebreak-isolate` — AMD fill on bcsstk25 (1.044× Eigen) — filed 2026-05-21

> **User-sanctioned optimization follow-on, NOT a defect or defer.** v2b AMD
> (`amd_order`) lands at 0.989× / 1.002× / **1.044×** Eigen-AMD `nnz(L)` on
> bcsstk13/24/25 — beats, ties, and 4.4%-above respectively. The ≤1.05× gate is
> met on all three. The bcsstk25 residual is an **un-isolated tie-break /
> iteration-order divergence from CSparse `cs_amd`** (all algorithmic features —
> approximate degree, mass elimination, supervariables, aggressive absorption,
> dense-node-last — are faithfully ported; the degree formula is algebraically
> identical).
>
> **Why it's not a defer (user-confirmed 2026-05-21):** AMD is a *heuristic* —
> faithful implementations (SuiteSparse `amd_2`, `cs_amd`, MATLAB, METIS-AMD)
> differ a few % matrix-to-matrix; "Eigen's exact number" is one impl's output,
> not a floor. Fill is a **downstream-perf knob (factor memory + flops), never
> correctness** — any permutation yields the identical solution. Across a corpus
> we're at parity-or-better (we beat Eigen on bcsstk13).
>
> **If revisited:** isolate the divergence (likely cs_amd's incidental node-scan /
> supervariable-principal order). Matching it bit-for-bit would re-pin D(ord)-5
> off "lowest-index principal" — a determinism-cleanliness trade for a few % on
> one matrix. **Real trigger:** an end-to-end v5 sparse-solve benchmark showing
> ordering fill (not the numerical kernels) is the bottleneck on a real workload.

### `v3b-1b-perf-followon-qr-block_reflector-consolidate` — unify qr.cpp + orgbr WY helpers — filed 2026-05-23

> **Mechanical dedup, NOT a defect.** v3b-1b-perf added shared
> `detail/block_reflector.hpp::build_block_t_from_vtv` (the `dlarft` factor) and
> uses it from `detail/orgbr.hpp`. `qr.cpp` still carries its own local copies of
> `build_block_t_from_vtv` + `materialize_panel_v` (anonymous-namespace, identical
> logic). Migrate qr.cpp to the shared header to remove the duplication — the same
> promote-then-consolidate pattern already filed for `dot_simd`
> (`v3b-1a-perf-followon-dot_simd-consolidate-eig_sym`). Low risk (relocate
> identical code; qr ctests gate it); deferred to keep the v3b-1b-perf slice's
> blast radius tight while the v3a-3/v3b tree is uncommitted.

### `v3b-1c-blocked-complex-bidiag` — blocked complex zgebrd for the at-scale complex SVD crush — filed 2026-05-23

> **Perf follow-on (correctness done), NOT a defect.** v3b-1c complex SVD is correct
> + gated (A=U S V^H <1e-9) and reuses the crushing real D&C/dbdsqr for the
> bidiagonal-SVD part, so it's competitive at moderate N. But the complex
> *bidiagonalization* (`bidiagonalize_complex`, zgebd2-class) is UNBLOCKED — at
> scale that's the bottleneck (exactly like the real path before v3b-1a-perf's
> blocked dlabrd). The at-scale complex speed-crush vs Eigen complex SVD + LAPACK
> zgesvd/zgesdd needs a **blocked complex `zgebrd`** (complex `zlabrd` panels +
> BLAS-3 trailing update via complex `gemm_parallel_auto`) + a complex `dorgbr`
> (zungbr) for the form-Q/P. Mirrors the real v3b-1a-perf/dorgbr work. Add a
> 4-column complex bench (Eigen BDCSVD-complex + LAPACK zgesvd/zgesdd) when this lands.

### `v3b-1c-svdvals-dqds-direct` — values-only complex svdvals via dqds — filed 2026-05-23

> **Perf follow-on, NOT a defect.** Complex `svdvals` currently routes through the
> full complex `svd` driver (computes singular vectors then returns the spectrum) —
> ~2× wasteful. The values-only path: complex bidiag → real (d,e) → dqds (`dlasq2`)
> directly, no vectors (mirrors the real `svdvals`). Factor the real svdvals'
> dqds-on-(d,e) extraction into a shared helper + call from the complex path.

### `v3b-3-nystrom-cholesky` — PSD-streaming Nyström variant of rsyev — filed 2026-05-23

> **Optional alternative, NOT a defect.** v3b-3 `rsyev` uses Rayleigh-Ritz
> (`QᵀAQ` + `eig_sym`) — general (any symmetric A) + reuses the eig_sym we beat
> Eigen+LAPACK with (D(svd)-15). The Tropp-2017 Nyström `C⁻ᵀ` form (`Y=AΩ`,
> shift ν, `B=ΩᵀY_ν` SPD, Cholesky `B=CCᵀ`, `F=Y_ν C⁻ᵀ`, svd(F)) is more
> accurate for PSD streaming/fixed-rank approximation. Add it as a `rsyev_psd`
> specialization if a PSD-streaming consumer arrives. Reuses Cholesky + trsm
> (both shipped v0e). Low priority — Rayleigh-Ritz covers the gate.

### `v3b-2-parallel-merges` — parallelize dlasd0's same-level merges — filed 2026-05-23

> **Optimization follow-on (we already win), NOT a defect.** v3b-2 D&C SVD already
> beats Eigen `BDCSVD` 1.59–3.21× / LAPACK `dgesdd` 1.37–4.55× at N=128–1024 via
> the parallel BLAS-3 back-transform + dlasd3 merge assembly (`gemm_parallel_auto`),
> with `dlasd0`'s recursion itself SERIAL. The independent same-level merge nodes
> (`dlasd1` calls within one `DO I=LF,LL` level) can run concurrently across
> `crd::jobs` — the lever that would widen the lead further (cores LAPACK/Eigen
> serial D&C lack at the merge level too). Per-merge Tlsf arena + bit-exact across
> worker counts. Deferred: the gate ("beat Eigen+LAPACK at scale") is already met;
> this is pure margin. Watch the frame-arena hazard ([[feedback_jobs_parallel_for_frame_arena_exhaustion]]).

### `v3b-2-svd-via-mrrr` — novel D&C-alternative SVD fork — filed 2026-05-23

> **Deferred research fork, NOT scope reduction.** The v3b-2 locked design flagged
> two routes to the full-SVD-at-scale crush: (1) Gu-Eisenstat D&C `dbdsdc`
> (chosen — the references' own algorithm, mirrors the winning Cuppen eigensolver),
> and (2) **SVD-via-MRRR** — form `J=[[0 Bᵀ][B 0]]` (2n×2n symmetric tridiagonal,
> ±σ eigenvalue pairs) and run the parallel MRRR (which already crushes LAPACK on
> the eigenvector path). The blocker: J's eigenvalues are EXACT ±σ multiples (every
> σ appears twice) → MRRR's cluster loop can't separate exact multiples → Gram-
> Schmidt fallback for every value defeats the O(n²) win. It needs a bespoke
> perfect-shuffle extraction (`[u;±v]/√2`) — a new driver, genuinely novel, real
> rabbit-hole risk. **Pursue only if Gu-Eisenstat D&C (v3b-2) does not reach the
> crush** (unlikely — Cuppen D&C already beats both Eigen + LAPACK).

### `v2c-small-n-analyze-constant-factor` — symbolic analysis 0.80× Eigen at n=2003 — filed 2026-05-21

> **Tracked perf follow-on, NOT a defect or defer.** v2c `symbolic_factorize` and
> its `nnz_l` analysis path beat Eigen `analyzePattern` at n=3562 (1.77×) and
> n=15439 (1.49×), but trail at the smallest test matrix bcsstk13 (n=2003) at
> **0.80×** (ours 1.65 ms vs Eigen 1.32 ms, Δ≈330 µs). The **pattern gate (the
> contract) passed bit-exact on all three** — this is purely the symbolic-analysis
> *timing* on one small matrix.
>
> **Why it's a constant-factor, not algorithmic (advisor-confirmed 2026-05-21):**
> our symbolic scales *better* than Eigen's — across the 7.7× n-range (2003→15439)
> our time grows 2.4× while Eigen's grows 4.4×. The crossover is between n=2003 and
> n=3562. That signature is fixed overhead, almost certainly `build_adjacency`
> (allocate + symmetrise `A∪Aᵀ` + per-row ascending sort) which is amortised away
> by n=3562. cs_counts itself is O(nnz(A)·α) — the asymptotically right choice
> (cheaper than a counting `cs_ereach` pass for high-fill matrices).
>
> **If revisited:** profile `build_adjacency` at small n; candidate wins —
> skip the re-sort when the input CSR is already sorted + symmetric (the SPD
> common case), or fuse the symmetrise into the etree pass. Low priority: 330 µs
> at the smallest problem size, on a step that is already faster than Eigen at
> every n that matters for sparse direct. **Real trigger:** a workload dominated
> by repeated small-matrix symbolic analysis (e.g. many-small-system batching).

### Transient MSVC LTCG internal compiler error on `win-shipping` `crd-sandbox.exe` link (observed 2026-05-13 v1-debts-paid sweep)

`scripts/full-sweep.ps1` returned 16/17 PASS during the v1-debts-paid verification sweep — only `win-shipping` failed, and only at the `crd-sandbox.exe` LTCG codegen phase with a fatal MSVC C1001 internal compiler error:

```
LINK : ... failed (exit code 0) with the following output:
Kod üretiliyor
D:\Dev\cerid\engine\config\src\config.cpp(245) : fatal error C1001: İç derleyici hatası.
(derleyici dosyası 'D:\a\_work\1\s\src\vctools\Compiler\Utc\src\p2\main.cpp', satır 263)
... link!DllGetObjHandler()+0x1fe99 ... CloseTypeServerPDB()+0x19fe ...
Access violation
ninja: build stopped: subcommand failed.
```

**The C1001 is not in our code.** `engine/config/src/config.cpp(245)` is plain `Config::load_from_file()` — pre-existing, unchanged in this slice. The line pointer is just where MSVC's whole-program optimizer happened to access-violate. Standalone retry of `cmake --build --preset win-shipping --target crd-sandbox` linked clean immediately with **no source change**. `win-clang-cl-shipping` (same shipping flags, different compiler) was green on the same sweep.

**Verdict:** non-deterministic MSVC LTCG internals bug, likely sensitive to the cross-TU template instantiation graph after v1 cluster's additions (new `BvhViewerCache`, `closest_point(Cylinder3)` + `closest_point(Tetrahedron)`, `tests/sandbox/test_showcase.cpp` adds new TU consumers of the same shared headers). One incident is upstream noise — the slice closes per `feedback_transient_msvc_ltcg_ice_accept.md`.

**Trigger to investigate:** recurrence on a subsequent sweep, or the same C1001 pointing at the same line range, would promote this from "transient" to "actionable upstream-workaround". Workaround candidates if it recurs:
1. `CRD_NOINLINE` on a suspected hot function in `config.cpp` or in the v1-cluster headers that LTO'd into sandbox.
2. `#pragma optimize("", off)` around `Config::load_from_file` (or the actual culprit if found).
3. Split a TU to reduce the LTCG working set.
4. Update MSVC toolchain (the canonical fix path but user-action).

**Recurrences (trigger met — now in "watch, workaround if it gets frequent" territory, still non-blocking per policy):**
- **2026-05-21 (hesap-sparse v1e-2 close):** same C1001 / Access violation in `link!DllGetObjHandler()`, this time on the `tests\sandbox\crd-sandbox-showcase-tests.exe` LTCG **link** (not config.cpp codegen) under `win-release`. 4/5 per-slice configs PASS; standalone `cmake --build --preset win-release` retry linked the same exe clean with no source change, then `ctest --preset win-release` = 2900/2900. So it remains the same non-deterministic LTCG-internals AV in the link phase, not our code. Pattern holds: it lands on whichever sandbox/showcase exe has the largest LTCG link working set. If it recurs again on the *next* sweep, apply workaround candidate 3 (split the showcase test TU) or 4 (toolchain update).
- **2026-05-21 (hesap-sparse v1f close), SAME exe again:** identical C1001 / `DllGetObjHandler` AV linking `crd-sandbox-showcase-tests.exe` under `win-release`; retry linked clean, `ctest --preset win-release` = 2909/2909. **Two consecutive win-release DoDs now ICE on the same exe → no longer "random noise"; it is reproducibly the largest-LTCG-link target.** ESCALATION: at the v1-close 18-config full sweep, if `crd-sandbox-showcase-tests` (or `crd-sandbox`) ICEs again, **apply workaround 3 (split `tests/sandbox/test_showcase.cpp` into 2–3 TUs to shrink the LTCG link set)** rather than just retrying — the retry tax is now predictable and will recur every sweep. Still non-blocking for the per-slice DoD (retry-PASS), but the workaround is now warranted, not deferred-by-default.

**Where referenced:**
- `docs/sessions/2026-05-13-v1-debts-paid.md` — the sweep transcript, decision to close on retry-success.
- `build/sweep-2026-05-13-v1-debts.log` (UTF-16) — original sweep log with the ICE.


### Phase 3.0 v1m Öbek system — three deferred follow-ups (2026-05-08)

The full Öbek system (ADR-0058) shipped across v1m1–v1m5b in twelve sub-slices. Three items were explicitly carved out as post-Phase-3.0 follow-ups so the v1m closure stayed focused.

1. **Hot-reload watcher with OCHN graph awareness** (v1m5c) — the öbek format already emits OCHN entries listing every transitive dependency (extends + nested) with FNV-1a 64 source-byte content hashes. What's missing: a filesystem watcher that consumes OCHN, detects upstream changes, and triggers transitive re-cook + atomic ResourceManager swap (matching the existing shader hot-reload pattern). Lands when filesystem-watching infrastructure is established (likely Phase 7 editor or earlier if a content workflow needs it).

2. **`obekc extract <source.obek.toml> --root <name> --output <new>` CLI tool** (v1m5c, ADR-0058 pillar 14 "Decompose") — extract a sub-graph rooted at a named entity into a new standalone `.obek.toml` file, with optional `--rewrite-source` to convert the original's inlined entities into a nested `obek = "..."` reference. Editor "make this a sub-prefab" operation. Needs its own binary entry point under `tools/`. Defer until the editor (Phase 7) or a real content workflow surfaces the need.

3. **InheritPolicy CoW: dense-buffer optimization** (v1m4b future) — v1m4b's CoW backend wastes `sizeof(component)` bytes per shared slot in the dense buffer (the bytes are unused for shared slots; only used after CoW write-break). For sizeof(component) >> sizeof(pool_idx), this dilutes the memory savings. A future optimization could allocate dense bytes lazily per-slot (e.g., a separate "owned slots only" dense buffer indexed by per-entity offset). Acceptable trade-off at v1m4b — pool-side dedup still gives N→1 sharing across instances, which is the dominant savings axis for the canonical "10k tree forest" workload.

**Where referenced:**
- `engine/scene/include/crd/scene/obek.hpp` — doc-block at the top of the file points at this debt entry.
- `docs/sessions/2026-05-08-scene-v1m5-revert-batch.md` — v1m closure session log.
- `docs/sessions/2026-05-08-scene-v1m4b-cow-backend.md` — pin #8 about wasted dense-buffer bytes.

---

### Phase 3.0 v1l cook_scene cooker — eight deferred follow-ups (2026-05-08)

`SceneCooker` + `scene_cooker_inline()` + `Transform`/six-relation built-in TOML readers + cooker-side propagation bake shipped in v1l. The authoring layer is in place; the following items are explicitly out of v1l scope.

1. **asset_cooker file-handler integration** — v1l ships the `SceneCooker` API but not the `.scene.toml` extension dispatcher. `tools/asset_cooker/src/cook_command.cpp`'s extension router does not yet route `.scene.toml` to `SceneCooker::cook_inline`. v1m (sandbox) or earliest content workflow will wire it; the API is ready and tested.

2. **Hierarchical entity addressing** — `[entity.player.weapon]` is rejected at cook time (test case in `test_scene_cooker.cpp`). A first-class child-as-nested-table syntax with cycle detection would simplify deep hierarchies; deferred to v1m+ once the sandbox surfaces a real authoring need.

3. **Per-instance prefab overrides** (v1k debt #5 reframed) — TOML `extends = "base.scene.toml"` with override blocks. The cooker is the right layer (instantiation-time merge). Reserved.

4. **Multi-file scene composition** — `[include = "level/region_a.scene.toml"]` recursive include with hot-reload-aware dependency tracking. Reserved for the streaming-load era (Phase 3.5+).

5. **Hot-reload of `.scene.toml`** — TOML watcher → recook → `SceneLoader.reload`. Same pattern as shader hot-reload but at the cooker layer. Reserved until the editor needs it.

6. **TOML schema migration** — when a component bumps its FourCC version, TOML migration tables let old `.scene.toml` files cook correctly without manual edits. Pairs with v1k debt #3 (binary-side migration).

7. **Compressed SCEN at the cooker** (v1k debt #7 picked up here) — CRDR supports zstd-compressed chunks (chunk-flag bit 0). v1l emits uncompressed. Multi-MB scenes will benefit; one-line flip in `SceneArtifactBuilder` once the cooker has size-based heuristics.

8. **Big-endian cooker output** (v1k debt #6 picked up here) — v1l SCEN is little-endian per CRDR. Cross-platform byte-order swap at cook time is a v1n+ concern.

**Where referenced:**
- `tools/asset_cooker/include/crd/cooker/scene_cooker.hpp` — doc-block points at this debt entry.
- `docs/sessions/2026-05-08-scene-v1l-cooker.md` — full session log with the propagation-bake fix and decisions.

---

### Phase 3.0 v1k SceneResource — seven deferred follow-ups (2026-05-07)

`SceneResource` + `SceneLoader` + `SceneArtifactBuilder` shipped in v1k. The persistence layer is in place; the following items are explicitly out of v1k scope. (Item #1 of the original eight closed by v1l on 2026-05-08; items #6 big-endian and #7 compressed SCEN repointed to the cooker layer in v1l's debt list above.)

1. **Streaming / incremental scene loading** — v1k loads-all-or-fail. Streaming visible-only entities (camera-frustum LOD, region-of-interest persistence) is Phase 3.5+. The current `SceneArtifactBuilder` filters at build time but the loader instantiates everything; partial-instantiation API is reserved.

2. **Schema migration** between SCEN versions — `kSceneSchemaVersion = 1` is fixed. v1n+ adds migration tables (v1 → v2 → ... transformer functions) once a layout change is needed. Pairs with v1l debt #6 (TOML-side migration).

3. **Entity-name lookup** post-load — finding a spawned entity by string name. Out of v1k scope; user-defined `Name` component or query-by-component is the path. v1m sandbox may want explicit name lookup; addressed there.

4. **Per-instance component overrides** — prefab+override pattern (instantiate scene, then override specific component values per entity). v1k loads verbatim. Now reframed as v1l debt #3 (cooker is the right layer).

5. **`World::mark_all_transforms_dirty()` helper** — convenience for callers loading a SCEN with stale world matrices who want propagation to re-derive. v1l's cooker bakes world matrices into SCEN bytes, so most callers no longer need this; the helper is still reserved if a use case appears.

**Where referenced:**
- `engine/scene/include/crd/scene/scene_resource.hpp` — doc-block points at this debt entry.
- `docs/sessions/2026-05-07-scene-v1k-scene-resource.md` — full session log with the eight decisions.
- `docs/sessions/2026-05-08-scene-v1l-cooker.md` — cooker session that closed item #1 and repointed #6/#7.

---

### Phase 3.0 v1j Transform — seven deferred follow-ups (2026-05-07)

`Transform` + `TransformPropagation` shipped in v1j with cross-domain robustness for games / robotics / aerospace / DAW. The following items are explicitly out of v1j scope; each has its own pickup phase or trigger condition.

1. **Polar decomposition for skewed Mat4** — `crd::math::to_trs` uses `from_mat3` on the post-scale-removal columns, which silently loses skew. True polar decomposition (SVD or iterative orthogonalisation) is reserved for a v1j+1 follow-up if a use case (CAD / mesh-import shear) appears. Documented in `to_trs`'s doc-block.

2. **TransformF64 (f64 precision) component + propagation system** — orbital / aerospace / atomic-resolution domains need f64 precision. Math layer already ships `crd::math::Transformd`. The v1j architecture supports it: register a `TransformF64` component + write a `TransformPropagationF64` `ISystem` that mirrors the f32 path. v1n's reserved-slot freeze test will verify the registration grammar accepts the custom type. v1k SceneLoader will accept it without changes.

3. **Parallel propagation** — single-threaded per ADR-0054. Phase 3.5 evolution once `par_each` over Query chunks lands. Per-subtree parallelism is straightforward (independent dirty roots → independent subtree DFS); each dirty root is one work-item.

4. **AttachedTo socket propagation** — Phase 3.2 (animation) ships an attachment-pose system that composes with TransformPropagation (sockets snap to bones).

5. **Per-system change tracking for `.changed<T>()`** — current ChangeDetect snapshot is "modified during current frame" (v1i pin). Cross-frame "what changed since my system last ran" needs per-system state. v1h+1 evolution.

6. **Auto-renormalize rotation policy** — v1j makes renormalize OFF-by-default. A registration trait (`AutoNormalizeRotation{}`) could opt-in per component. Reserved slot if accumulated drift becomes visible in real workloads.

7. **`set_rotation_look_at` direction convention** — current implementation uses (right, up, -forward) columns matching the right-handed convention. Some domains (aerospace yaw-pitch-roll) need (forward, right, up) variants. Reserved as a follow-up trait or alternative API if a domain needs the explicit convention.

**Where referenced:**
- `engine/scene/include/crd/scene/transform.hpp` — doc-block points at this debt entry for items 2 and 6.
- `engine/math/include/crd/math/quat.hpp` — `to_trs` doc-block points at item 1.
- `docs/sessions/2026-05-07-scene-v1j-transform-propagation.md` — full session log with the seven decisions.

---

### TLSF allocator — three deferred enhancements (D-001-a, 2026-05-07)

`TlsfAllocator` ships in production-grade form: arbitrary alignment, O(1) operations under ASan stress (1000 iterations × 16/32/64/128/256-byte alignments), `try_allocate` non-throwing path. Three enhancements are consciously deferred:

1. **Conte's 8-byte block-header overlap trick.** Saves 8 B per allocation by overlapping the next block's `prev_phys_block` field with the previous block's payload tail. Documented in `docs/sessions/2026-05-07-detour-D-001a-tlsf.md`. Layout change is high-risk; the 8-byte saving is marginal at engine scale (1000 allocations of 100 bytes each saves ~8 KB). Pick up if memory pressure ever justifies — likely never.

2. **32-bit pointer support.** Cerid CI is 64-bit. Constants (`kFlIndexMax = 32`, the `unsigned long long` cast in `fls_size`) assume 64-bit. Adding template parameterization on pointer width adds bug surface for zero current benefit. Pick up if a 32-bit embedded target ever appears.

3. **Multi-threaded TLSF.** `IAllocator` base class documents "not thread-safe by default; hand them out per-thread or wrap them yourself" — this is the engine-wide convention. Lock-based TLSF kills the O(1) latency claim; lock-free TLSF is research-tier (Marotta et al. 2018). The standard scaling pattern is per-thread arenas. Don't pick up — this isn't TLSF-specific debt; it's a project architecture decision.

**Where it's referenced:**
- `engine/memory/src/allocators/tlsf_allocator.cpp` — current implementation comments document each deferred item at the relevant code site.
- `docs/sessions/2026-05-07-detour-D-001a-tlsf.md` — full design rationale.

---

## Long-term deferred

- **Stress `[.soak]` nightly lane** (deferred by detour D-002, 2026-05-12) — `tests/stress/`
  ships four `[.soak]`-tagged tests (TLSF churn, freeze + parallel_for, `ConcurrentQueue`
  MPMC, `AtomicArray`) with deliberately huge iteration counts (catch a 1-in-10M torn-write
  / false-share that the bounded variants miss). They're Catch-`[.]`-hidden so CI `ctest`
  skips them; today they only run on demand via `crd-stress-tests "[.soak]"`. Wanted: a
  scheduled nightly CI job (and a place to add future `[.soak]` tests as v3–v6's primitives
  pick up consumers). Until then, run the soak suite manually before relying on a new
  consumer of these primitives.
- **Concurrent hash map** (deferred by detour D-002, 2026-05-12) — a split-ordered /
  Cliff-Click-style lock-free hash map is genuinely hard and should not be built
  speculatively. D-002 ships `crd::containers::ConcurrentQueue<T>` (MPMC) and
  `crd::containers::AtomicArray<T>` (bounded atomic-append); a concurrent map lands
  only when a concrete consumer demands it. Until then: per-fiber scratch maps + a
  serial merge, or a `ConcurrentQueue` of update-requests drained by one fiber.
- **Thread-safe / sharded global allocator** (deferred by detour D-002, 2026-05-12) —
  `TlsfAllocator` and the pool/linear/stack allocators are single-threaded-by-contract
  and will *not* get a lock bolted on. If a shared cross-fiber heap is ever needed it is
  a new sharded/thread-caching allocator (tcmalloc-style: per-thread free lists + a
  locked central heap), built then, not now. `RefCounted` objects whose final release can
  occur off the creating thread must be backed by a thread-safe allocator or a
  deferred-free queue.

- **Multi-viewport ImGui** — Vulkan multi-viewport has known rough edges.
  Single-viewport docking only until `crd-ui` ships (planned Phase 5+).
  At that point, game/editor surfaces move to `crd-ui`; ImGui stays debug-only
  and multi-viewport is no longer needed.

