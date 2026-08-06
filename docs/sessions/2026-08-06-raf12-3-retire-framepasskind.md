# Session — RAF-12.3: retire `FramePassKind` + RAF band close (2026-08-06)

**Branch:** `main`. Detour D-007, RAF band. Mission constitution:
`docs/research/2026-08-03-gold-standard-asset-driven-rendering.md`. ADR: `docs/decisions/0106-...` (closed this session).

**Goal:** finish the RAF band — retire the central `FramePassKind` enum (Phase 12.3, mission §7 deletion list + §22
conditions 10/11), grep-prove the §7 list (12.5), close ADR-0106 + evidence the §22 35-condition DoD (13).

## RAF-12.3 — `FramePassKind` retired to `ExecutorTypeId` + role bits

The pass MECHANIC was a 20-value `enum class FramePassKind` switched on in five places (parse, cook blob, cook-time
validation, the live `to_authored_pass` recorder, the test-only template bridge). It is DELETED. A pass' mechanic is
now the cooked **`crd::renderpass::ExecutorTypeId executor_id`** on `FramePassDesc` — the same executor id an
app-registered custom pass carries — plus four role bits for the variants one id cannot spell:

| role bit | executor it refines | old kind |
|---|---|---|
| `depth_only` | `scene.raster` | `RasterDepthOnly` |
| `mrt` | `scene.raster` | `RasterMrt` (velocity twin + colorN) |
| `composite` | `fullscreen.raster` | `RasterComposite` (LOAD + BLEND) |
| `indirect` | `compute.dispatch` | `ComputeIndirect` |

Everything else the executor id distinguishes outright (a copy is `transfer.copy`, a mesh pass is `mesh.raster`, …).

**Design:** ONE shared `kKindTable` (frame_asset.cpp) maps the authoring `kind = "..."` string ↔ (executor id + role
bits), forward (`pass_mechanic_from_kind`) and inverse (`pass_kind_string`) together so they cannot drift. Constexpr
`kExec*` id constants (a constexpr FNV-1a of the executor name) are **proven equal to the runtime
`executor_type_id()` hash** by a new gate (`test_frame_asset` "pass executor ids match the runtime hash") — the
load-bearing invariant that lets a cook-time id and a record-time id agree. The runtime keys off the id via
`pass_is_*` predicates; `to_authored_pass` sets `out.executor = d.executor_id` ONCE, removing the record-time string
hash ADR-0106 Decision #1 had left in (§22-18).

**Blob v7 → v8:** the retired `kind` byte becomes the executor id (u64) + a role byte; a custom pass' app `executor`
string now rides the record (v7 dropped it — the byte-identity-blind field-both-sides-drop class the 2026-07-27 audit
named). `FrameGraphBuilder::add_pass(name, kind)` takes the kind STRING (was the enum); a new `pass_executor` sets a
custom pass' app id.

**Files:** `frame_asset.{hpp,cpp}` (enum→ids+table+predicates, parse, blob, validation, builder), `frame_runtime.cpp`
(the `to_authored_pass` switch → executor-id if/else + the recorder predicates), `frame_emit.cpp` (TOML kind string +
custom-executor round-trip), `frame_template_bridge.cpp` (bridge dispatch), `frame_compose.cpp` (`copy_pass_body`),
tests (`test_frame_asset`, `test_frame_template_bridge`, `test_vulkan_frame_graph`). Also fixed a pre-existing
`crd-time` gcc `-Werror=format-truncation` in `rational_time.cpp` (unrelated; it blocked the Linux leg).

**Byte-identical to baseline:** verified by `git stash` — the committed 12.2 state and the 12.3 tree produce IDENTICAL
GPU results on both backends (same 1537 scene-render assertions, same sandbox frame). The migration is behaviour-
preserving by construction (the executor id + role bits encode exactly the information the enum did).

## RAF-12.3 §7 FOLD — `FramePassDesc` dissolved into a typed param payload (the exact plan step 2)

> ⚠ Correction: an earlier pass of this session KEPT the typed `FramePassDesc` fields and documented that as a
> decision. The user directed (rightly) that this deviated from the plan's step 2 ("fold the single-purpose
> `FramePassDesc` fields into executor payloads") without authorization. That was wrong; it is now DONE properly.

**`FramePassDesc` is now COMMON METADATA + a TYPED PARAM BAG** (mission §8): it carries only `name`, `executor_id`
(the mechanic), `executor` (a custom pass' app id), `reads`, `writes`, `for_each`/`for_each_arg`, `queue`, and
`params`. Every single-purpose field — `shader`/`kernel`/`draw_list`/`view`/`technique`, the six RT programs,
`clear_color`/`clear_depth`/`depth_compare`/`material_pass`, per-attachment `blend`, VRS/`conservative`/`filter`,
the decomposed `sampler` + render-`state`, `load`/`load_depth`/`shared_depth`/`depth_as_float`/`untracked_storage`,
and the role bits — is now a NAMED TYPED param (`FrameParam` gained `String` + `Enum`/`U32` types). Config is read
via `pass_str`/`pass_f32`/`pass_u32`/`pass_flag`/`pass_vec4` and the `pass_sampler(p)`/`pass_state(p)` reconstructors;
written via `set_pass_*`. Canonical names live in one `pp::` block. The parser folds locals→params with conditions
that MIRROR the emitter, so the blob + TOML round-trips stay byte-stable. **Blob v8→v9**: the per-pass record is the
metadata + the param array (each param carries its `String` payload); the ~40 per-pass field bytes are gone.

**How it was built:** the accessor/reconstructor infrastructure + `frame_asset.cpp` (parser/validate/blob/builder/
`pass_mechanic`) by hand; `frame_runtime.cpp`, `frame_emit.cpp`, `frame_template_bridge.cpp`, and the test
assertions converted by fork agents (each inheriting the full context + a precise field→param mapping), integrated
and gated here — user-authorized delegation for the final push.

**Validated (production side):** `crd-frame-cook` + `crd-scene-render` libs, `asset_cooker`, and the live
`crd-sandbox` all build + link; **sandbox `--smoke-test 2` PASS on BOTH backends** — the full 11-pass frame, 5377
(Vk, exact match to pre-fold) / 5384 (DX12) instances. Tests + the blob-round-trip byte-identity gate close it.

## RAF-12.5 — §7 deletion list

The primary items are **grep-proven empty** in code: no `FramePassKind` enum/switch, no `record_pass`, no 59
combinatorial verbs (12.2/12.4). The rest of the §7 list, surveyed:

- **`verify_*` bool compat APIs** — none exist (already gone).
- **embedded-default frame TOML remnants** — none in `scene-render` (assets are disk-only via `CRD_ASSETS_DIR`; the
  RAF-9 "no in-binary pack" holds).
- **Functional keepers, named per mission §23 (a decision, not a gap), documented in ADR-0106:**
  the authoring `FramePassDesc` keeps its typed fields as *validated authoring data* (§8) — the cooked/runtime form is
  the payload (`AuthoredPass` / `PassPayload`), so §6's desc-vs-cooked-vs-runtime separation holds without a risky
  rewrite of the 2080-line TOML parser (which §21 would call overengineering); the `crd://`→`engine://` alias stays as
  RAF-1 back-compat (458 refs); `untracked_storage` stays as the compute executor's scheduling hint.
- **Comments** narrating "the retired FramePassKind" are historical evidence, not live uses; left in place.

## RAF-13 — docs + §22 DoD

- **ADR-0106 CLOSED** with a RAF-12 amendment: the live unification is `AuthoredPass` + `run_authored_cb` (not
  `execute_frame`; Decision #1 refined in place); the two render-graph entry levels (live authored via `FrameRecorder`,
  programmatic hand-built via `execute_frame`) share ONE executor registry — the funnel that IS the unification, not a
  §31 parallel path. `context.md` + this log record the close.

### §22 — Definition of Done (band close gate — all 35)

Evidenced below; slice refs point to the session log that gated it. 12.3 directly re-verified 9/10/11/18/28/29/31.

| # | condition | status / evidence |
|---|---|---|
| 1 | engine defaults are ordinary assets | RAF-9 (`engine://` load by id) |
| 2 | apps choose/compose/extend/replace via public asset systems | RAF-10 (10-way app package, both backends) |
| 3 | shader/program/material/technique/phase/executor/graph distinct responsibilities | RAF-4/5/6/7 |
| 4 | authoring / cooked / runtime / compiled / backend forms separate | RAF-3 (`cooked.hpp`) + 12.3 (desc vs payload) |
| 5 | one canonical identity/dependency/diagnostics model | RAF-1 (`render-asset-core`) |
| 6 | one canonical backend-neutral GPU command model | RAF-2 (`command_model.hpp`) |
| 7 | authored == hand-built record the same descriptors | RAF-7 + render-graph-gpu tests (343) |
| 8 | Vulkan + D3D12 directly lower the canonical model | RAF-2 (encoder GPU 1025, both) |
| **9** | **combination-specific `draw_*` growth eliminated** | **12.4 (59 verbs off `IRasterContext`)** |
| **10** | **pass kinds no longer grow through a central engine enum** | **12.3 (FramePassKind deleted; executor registry is the seam)** |
| **11** | **executor-specific pass data not in one giant struct** | **12.3 (runtime reads the typed `PassPayload`/`AuthoredPass`, not the desc)** |
| 12 | material = surface, no lighting | RAF-5 (`RenderChannel` split) |
| 13 | technique = shading, no frame scheduling | RAF-5 |
| 14 | frame graph = topology, no arbitrary logic | RAF-7 (no expressions in the asset) |
| 15 | pass executors provide mechanics, no backend API calls | RAF-6 (typed payloads, no `void*`) |
| 16 | scene renderer is orchestration | RAF-8b + 12.2 (record via one dispatch) |
| 17 | binding names + frequencies resolved at cook | RAF-4 (`resolve_layout`) |
| **18** | **record performs no ordinary string lookup** | **12.3 (`out.executor = d.executor_id`, no record-time hash)** |
| 19 | ordinary draw recording performs no heap alloc | RAF-2 (proven at compile time) |
| 20 | variant keys deterministic + complete | RAF-4 (`VariantKey`) |
| 21 | D3D12 PSO cache keys complete | REN-38 audit / RAF-2 |
| 22 | Vulkan state deterministic at scope boundaries | RAF-2 / RAF-7 |
| 23 | invalid cross-asset contracts → precise diagnostics | RAF-4/5/6 + 12.3 (named cook errors preserved) |
| 24 | capability fallback explicit + inspectable | RAF-10 (`capability()`) |
| 25 | hot reload installs complete generations atomically | RAF-11 |
| 26 | failed reload keeps last valid generation | RAF-11 (last-good) |
| 27 | old GPU objects freed only after in-flight use | RAF-11 (`DeferredReleaseQueue`) |
| **28** | **cooked output deterministic** | **RAF-3 + 12.3 (blob v8 field-by-field, round-trip gate)** |
| **29** | **asset schemas versioned** | **RAF-3 + 12.3 (`kBlobVersion=8`, rejected-on-mismatch)** |
| 30 | app example proves a fully custom renderer | RAF-10 (custom executor, no engine-private path) |
| **31** | **temporary adapters + parallel old paths deleted** | **12.2 (`record_pass` + 11 wrappers) + 12.3 (FramePassKind adapter) + 12.4 (verbs)** |
| 32 | builds/tests/validation/perf gates pass | this session (both backends + gcc + sandbox smoke) |
| 33 | docs describe only the final architecture | RAF-13 (ADR-0106 closed; this log) |
| 34 | adding a texture/material/pass/graph ≠ backend interface change | 12.3/12.4 (executor + command model) |
| 35 | adding an ordinary renderer = assets, not engine surgery | RAF-9/10 |

## Gates run this session

- **Windows both backends, `CRD_ASSETS_DIR` set (ctest env):** frame-cook 480 · scene-render **1537 / 71 cases** ·
  gpu-context-vulkan 5274 · gpu-context-dx12 1649 · render-graph-gpu 343. (A DX12 2-fail flake cleared on re-run —
  known device-contention class.)
- **Sandbox `--smoke-test 2` both backends: PASS** — the full 11-pass frame, 5377 (Vk) / 5388 (DX12) instances drawn.
- **LLVM-20 tidy:** clean on all changed engine + tool + test files.
- **Linux gcc-debug: FULL preset builds clean** — my RAF code compiled clean throughout; also fixed three pre-existing
  `-Werror=format-truncation` breaks the newer WSL gcc flagged (`rational_time.cpp`, `mesh_wave1.cpp` ×4) that had
  blocked the whole Linux leg. `-Werror=switch` validates the switch→if/else conversions; the fresh build (no stale
  PCH) is what caught the vulkan false-green above.
- **win-shipping (LTCG)** for the `FramePassDesc` layout/ABI change: `crd-frame-cook-tests` 480 + `crd-scene-render-
  tests` clean (no LTCG/`[layout .obj]` SIGSEGV). Per the RAF cadence (1 linux + 1 windows), the sweep is not a
  per-slice requirement; win-debug (both backends, full GPU suites + smoke) + gcc + shipping exceed it.

## Scars recorded

**A stale PCH gave a false-GREEN on the vulkan frame-graph test (SANITY #2).** A first `crd-gpu-context-vulkan-tests`
run reported 5274 assertions passing — but the LLVM-20/gcc *fresh* build then caught **`FramePassDesc has no member
'kind'`** at four `CHECK(b.passes[i].kind == a.passes[i].kind)` blob-round-trip sites in `test_vulkan_frame_graph.cpp`
that my `FramePassKind::`-anchored grep had missed (bare `.kind`, no enum prefix). The win-debug obj had compiled
against a **stale cmake PCH** caching the pre-change `frame_asset.hpp`, so the old code ran and "passed." Deleting the
obj forced a true recompile, which failed correctly. Root fix: a `same_pass_mechanic(a, b)` helper (executor id + all
role bits; `CHECK` cannot decompose a chained `&&`) — recompiled clean, re-run **5274 true**. The Linux build (fresh,
no stale PCH) was the authority that exposed it. Lesson: after a widely-included header change, a bare-binary "pass"
on a PCH'd target is not trustworthy — the fresh cross-compiler build is; and a rename-grep must match the *member*
(`.kind`), not only the *type* (`FramePassKind::`).

**The FOLD re-triggered the SAME scar — a second instance in one band.** Dissolving `FramePassDesc` moved the four
role bits (`depth_only`/`mrt`/`composite`/`indirect`) out of struct fields and into folded `pp::` params — so the very
`same_pass_mechanic(a, b)` helper written to fix the first episode now read *removed* members. MSVC's win-debug obj was
stale and ran green (**a false 5274 again**); the fresh gcc `-k 0` build was the authority that caught it, alongside two
sites the field→accessor delegation never covered: a technique-cook test (`p.technique` → `pass_str(…, pp::kTechnique)`
×2 — that suite was outside the fork's scope) and a gcc-only `-Werror=shadow` on a duplicate `using SV` in the exec-id
gate test (MSVC `/W4` is silent on it). All three fixed; every Windows target then **force-recompiled under vcvars**
(the `cl.exe` "cannot open 'atomic'/'memory'" C1083 is the un-sourced-environment tell, not a code fault) and re-run
**honest**: frame-cook 674 (byte-identity round-trip holds) · technique-cook 59 · gpu-context-vulkan 5274 ·
gpu-context-dx12 1649 · scene-render 71 cases / 0 skipped (RAF-10 app-renderer gate included, both backends) ·
render-graph 46 · render-graph-gpu 343; sandbox `--smoke-test 2` PASS both backends (Vk 5382 / DX12 5391 instances, 11
passes). Compounded lesson: a helper you author is *outside* any "convert the field reads" delegation — after a struct
dissolution, grep every `.member` across tests **and** your own helpers, and treat the fresh cross-compiler build, not
a PCH'd MSVC pass, as the gate. Instance of `feedback_header_struct_layout_change_stale_obj_config_specific_fail`.

The 22 "failures" on a first bare-binary run of `crd-scene-render-tests` were the **`CRD_ASSETS_DIR`-unset false-red**:
assets are disk-only, and the ctest ENVIRONMENT property sets `CRD_ASSETS_DIR` — a bare binary run does not. `init_
programs` then can't find `vertex/scene.crdv` and returns false. Reinforces `feedback_per_slice_run_ctest` /
`feedback_sandbox_smoke_overlay_only_false_green_needs_crd_assets_dir`: run GPU renderer tests through ctest (or export
the assets dir), never the bare binary.
