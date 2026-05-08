# 2026-05-08 — Phase 3.0 v1l: cook_scene cooker (`.scene.toml` → SCEN)

**Status at start:** Phase 3.0 v1k shipped 2026-05-07. SceneResource + SceneLoader + SceneArtifactBuilder + `World::instantiate_scene` in place. Six-config 758/758 (post-renderer additions) / 17 smokes. Scene tests 225 / 34783.

**Status at end:** v1l shipped — the **authoring layer**. Human-edited `.scene.toml` files cook to deterministic SCEN bytes via the same `SceneArtifactBuilder` shipped in v1k. Built-in readers cover `Transform` + the six built-in relations; user-defined components register their own TOML readers. The cooker bakes world matrices in a temp World by running `TransformPropagation::step()` before serialising — loaded SCEN packs come back with hierarchical world matrices already correct. Six-config 758/758 / 755 release / 17 smokes. 17 cooker tests / scene-cooker pass cleanly across all configs.

**This is the on-ramp for v1m (sandbox renderer integration) and the offline content pipeline writ large.** A scene now travels: `.scene.toml` → cooker → SCEN bytes → `instantiate_scene` → live World.

---

## Goal of this session

Land the authoring half of ADR-0055:
1. **`crd-cooker` extensions** — TOML reader registry, built-in readers, SCEN artifact emission via `SceneArtifactBuilder`.
2. **`SceneCooker` class** + `scene_cooker_inline()` free function — public API for tests and the future asset_cooker file handler.
3. **Built-in TOML readers** — `Transform` (TRS + cached world placeholder) + six built-in relations (entity-name → file_idx resolved at cook time).
4. **Cooker-side propagation** — run `TransformPropagation` in the temp World before `SceneArtifactBuilder.build` so the cooked SCEN carries correct world matrices for hierarchical scenes.
5. **Determinism** — TOML walked in document order; per-entity component fields walked in alphabetical key order; same TOML → bit-exact SCEN bytes.

## What shipped

### New files

```
tools/asset_cooker/include/crd/cooker/scene_cooker.hpp     ~160 LOC — SceneCooker, SceneCookContext, ComponentTomlReaderFn, free scene_cooker_inline, read_transform_from_toml
tools/asset_cooker/src/cook_handlers/scene.cpp             ~675 LOC — three-pass cooker, built-in readers, FNV-keyed name lookup, propagation-bake, SCEN emission
tests/scene_cooker/CMakeLists.txt                          —          link list + Catch2 discover
tests/scene_cooker/test_scene_cooker.cpp                   ~540 LOC, 17 cases
```

### Modified

- `tools/asset_cooker/CMakeLists.txt` — added `crd-math`, `crd-scene` to the public link list; added `TOML_EXCEPTIONS=0` compile definition (matches `crd-config`'s no-exception toml++ pattern); added `tomlplusplus` PRIVATE link.
- `tests/CMakeLists.txt` — `add_subdirectory(scene_cooker)`.

### TOML schema

```toml
# Per entity: a [entity.NAME] table. Components are inline tables;
# relations are strings (single target) or arrays of strings (multi).

[entity.arm]
Transform = { translation = [0.0, 1.5, 0.0], rotation = [0,0,0,1], scale = [1,1,1] }

[entity.hand]
Transform = { translation = [0.4, 0.0, 0.0] }
ChildOf   = "arm"

[entity.tooltip]
Transform = { translation = [0.0, 0.1, 0.0] }
AttachedTo = "hand"

[entity.controller]
Targets   = ["arm", "hand"]   # non-acyclic — array allowed
PossessedBy = "player"        # non-acyclic — array allowed

# Acyclic relations (ChildOf / AttachedTo / Owns / DependsOn) reject
# array form at cook time.
```

### Three-pass cooker

```
Pass 1 — collect entities:    walk [entity.*] tables in document order;
                              spawn each into the temp World; map name → EntityId.

Pass 2 — apply components:    walk component fields in alphabetical key order
                              (advisor pin #8 determinism); each component dispatches
                              to its registered TOML reader, which writes raw bytes
                              into a stack buffer; cooker installs via the storage
                              backend's add path.

Pass 3 — install relations:   walk relation fields; resolve entity-name → file_idx
                              via FNV-hash map; reject array form for acyclic
                              relations; reject unknown target names. Multi-error
                              accumulation: cooker reports every error before failing.

Bake — propagation step:      mark every Transform-bearing entity dirty;
                              run World::step(1/60) → TransformPropagation runs
                              in PreRender phase → world matrices baked.

Emit — SceneArtifactBuilder.build(world) → CRDR `'SCEN'` bytes.
```

### Five architectural decisions pinned

1. **Cooker bakes world matrices, not the loader** (the propagation-bake fix). The temp World runs `TransformPropagation::step()` before `SceneArtifactBuilder.build`, so SCEN packs carry correct hierarchical world matrices. Loaded scenes are immediately renderable without the consumer running propagation manually. Verified by `"Cooked hierarchy + step propagation"` test case (child world `c3.x == 11.0F` after parent translation 10 + child translation 1).

2. **Component reader registry is content-driven** (advisor pin #4). Each persistable component registers a `ComponentTomlReaderFn` keyed by its TOML field name. Built-ins (`Transform` + six relations) are auto-registered by `register_builtin_readers()`. User-defined components register before cooking via `register_component_reader<T>(name, reader, fourcc, version)`. The cooker has no hard-coded knowledge of any component type.

3. **Multi-error accumulation** (advisor pin #3). All cook errors collect into `Array<CookError>{message, line, column}` before the cooker fails. `register_component_reader` and the parse pass never short-circuit; the cooker reports every problem found in one run. Verified by `"Multiple errors accumulate"` test case (3 distinct errors emitted).

4. **Determinism via document-order + alphabetical key walk** (advisor pin #8). Entities take file-local indices in TOML document order; per-entity component fields apply in alphabetical key order. Combined with the existing CRDR sort-by-FourCC chunk order and SCEN slot-order entity walk, the same TOML → bit-exact SCEN bytes. Verified by `"Determinism: identical TOML produces bit-exact SCEN bytes"` test case (FNV hash of full byte stream over two cook runs).

5. **Acyclic relations reject array form at cook time**. `ChildOf`, `AttachedTo`, `Owns`, `DependsOn` accept only single-target string values; supplying `["a", "b"]` emits a diagnostic and fails the cook. `Targets` and `PossessedBy` accept arrays. Verified by `"Acyclic relation rejects array form"` test case.

### `Transform` TOML schema and reader

The reader (`read_transform_from_toml`) walks the inline-table fields:
- `translation` — `[f32, f32, f32]`, default `[0,0,0]`.
- `rotation` — `[x, y, z, w]` quaternion (w last per Cerid math convention), default identity `[0,0,0,1]`.
- `scale` — `[f32, f32, f32]`, default `[1,1,1]`. Scalar `scale = 2.0` is also accepted (uniform scale shorthand).

Missing fields fall back to defaults; type mismatches accumulate errors. The reader writes `Transform{}` bytes directly; the propagation-bake step computes the cached world matrix.

### Six built-in relation readers

`ChildOf`, `AttachedTo`, `Owns`, `Targets`, `DependsOn`, `PossessedBy` register with the cooker at `register_builtin_readers()`. Each pairs a TOML key with the FourCC declared in `engine/scene/include/crd/scene/serialize.hpp`. Relations are not "read into bytes" — the cooker resolves `name → EntityId` via the entity-name FNV map and calls `World::add_relation_via_id(component_id, src, tgt)` directly. The relation's serialize trait (registered in `register_builtin_relations`) handles the SCEN-side persistence at `SceneArtifactBuilder.build` time.

### Test matrix (17 cases / scene-cooker)

| # | Case | What |
|---|---|---|
| 1 | Empty TOML cooks to a valid SCEN with zero entities | Sanity floor |
| 2 | Single entity with default Transform round-trips | Basic happy path |
| 3 | Two entities with explicit Transforms preserve TRS values | Multi-entity, value preservation |
| 4 | ChildOf hierarchy (parent + child) | Built-in acyclic relation |
| 5 | All six built-in relations cook + round-trip | Coverage of relation registry |
| 6 | Targets array `[a, b]` — non-acyclic with multiple targets | UPSERT model: result = 1 relation per Tag (latest wins) |
| 7 | Acyclic relation rejects array form | Negative test for ChildOf/AttachedTo/Owns/DependsOn |
| 8 | Unknown component key fails cook with diagnostic | Negative test, error formatting |
| 9 | Missing relation target name fails cook with diagnostic | Negative test, name resolution |
| 10 | Type mismatch in Transform field fails cook with diagnostic | Negative test, reader-side error |
| 11 | Hierarchical entity name `[entity.player.weapon]` rejected | Reserved namespace decision |
| 12 | Multiple errors accumulate (3 distinct errors emitted) | Multi-error semantics |
| 13 | Default Transform preserved verbatim through cook + load | Identity round-trip |
| 14 | Determinism: identical TOML → bit-exact SCEN bytes | FNV hash over byte stream, two runs |
| 15 | Cooked hierarchy + propagation post-load | The propagation-bake fix |
| 16 | Builder-control + direct-control isolation tests | Diagnostic harness for fix #15 |
| 17 | 100-entity stress test | Cook + round-trip end-to-end |

### Critical fix: cooker-side propagation bake

While building the test matrix, `"Cooked hierarchy + propagation post-load"` failed: a child entity at local `[1,0,0]` under a parent at local `[10,0,0]` came back with `world.c3.x == 1.0F` instead of `11.0F`. Investigation:

- **Direct-built control** (skip cooker, build World directly, run step, check matrix): passed.
- **Builder-control** (direct World → `SceneArtifactBuilder.build` → `instantiate_scene` → step on target World): passed.
- **Cooker path** (TOML → cooker → SCEN → `instantiate_scene` → step on target World): failed.

Root cause: the cooker's temp World inserted `Transform` components directly via the backend (`add_component<Transform>(e, t)`) — skipping `set_translation` etc. The `TransformDirtyFlag` SparseSet was never marked, and the cooker never ran `step()`. So `SceneArtifactBuilder` serialised the `Transform`'s `world` field at its default (= the local matrix), and the loaded scene's child world matrix was wrong from the start.

Fix (`scene.cpp:540-655`): after pass 3 completes, the cooker walks every entity name, marks each Transform-bearing entity dirty via `World::mark_transform_subtree_dirty`, and calls `world.step(1.0/60.0)` before `SceneArtifactBuilder.build`. This requires registering `TransformPropagation` as a system in the temp World (`setup_temp_world`):

```cpp
void setup_temp_world(crd::scene::World& w)
{
    w.register_component<crd::scene::Transform>(crd::scene::transform_serialize_trait());
    w.register_component<crd::scene::TransformDirtyFlag>(crd::scene::StorageHint::SparseSet);
    w.register_builtin_relations();
    w.register_system(std::make_unique<crd::scene::TransformPropagation>());
}
```

Why bake at cook time, not load time? Three reasons:
1. **Idempotent SCEN content.** A SCEN that already carries correct world matrices doesn't depend on the loader running a system. Networking/replay/snapshot consumers can't assume the target World has propagation registered.
2. **Authoring sanity.** A scene authored with hierarchical TRS should "just work" when loaded — no consumer-side `step()` required for the rendering pipeline to read correct world matrices.
3. **Deterministic by construction.** Bake order is fixed by the cooker's pass schedule + propagation's deterministic walk; bytes are bit-exact across runs.

### Eight follow-ups pinned in `docs/debt.md`

1. **asset_cooker file-handler integration** — v1l ships the `SceneCooker` API but not the file-extension dispatcher. The `.scene.toml` extension is not yet registered with `tools/asset_cooker/src/cook_command.cpp`'s extension router. v1m or earliest content workflow will wire it.
2. **Hierarchical entity addressing** — `[entity.player.weapon]` is rejected at cook time. A first-class child-as-nested-table syntax with cycle detection would simplify deep hierarchies; deferred to v1m+.
3. **Per-instance prefab overrides** — TOML `extends = "base.scene.toml"` with override blocks. Same shape as v1k debt #5.
4. **Multi-file scene composition** — `[include = "level/region_a.scene.toml"]`. Reserved with hot-reload-aware dependency tracking.
5. **Hot-reload of `.scene.toml`** — TOML watcher → recook → `SceneLoader.reload`. Same pattern as shader hot-reload but at the cooker layer.
6. **Schema migration in TOML** — when a component bumps its FourCC version, TOML migration tables let old `.scene.toml` files cook correctly without manual edits.
7. **Compressed SCEN at the cooker** — v1k debt item #7 sits naturally at the cooker. Multi-MB scenes will benefit; single line in `SceneArtifactBuilder` to flip the chunk-flag bit.
8. **Big-endian cooker output** — v1k debt item #6, picked up at the cooker layer when cross-platform output appears.

### Six-configuration green (post-v1l, 2026-05-08)

- win-debug:          758/758
- win-relwithdebinfo: 758/758
- win-release:        755/755
- win-asan:           758/758
- win-clang-cl:       758/758
- win-tidy:           ✅ build clean

17/17 headless smokes per non-tidy config. Scene-cooker tests: 17 cases (added). Total scene + scene-cooker: 242 cases / ~35200 assertions.

---

## Files touched

```
tools/asset_cooker/CMakeLists.txt                          modified
tools/asset_cooker/include/crd/cooker/scene_cooker.hpp     created (~160 LOC)
tools/asset_cooker/src/cook_handlers/scene.cpp             created (~675 LOC)
tests/CMakeLists.txt                                        modified
tests/scene_cooker/CMakeLists.txt                           created
tests/scene_cooker/test_scene_cooker.cpp                    created (~540 LOC, 17 cases)
docs/sessions/2026-05-08-scene-v1l-cooker.md                created (this file)
docs/phases/phase-3.0-scene-ecs.md                          updated (v1l shipped, v1m next)
docs/debt.md                                                updated (eight v1l follow-ups added; v1k debt #1 closed)
CONTEXT.md                                                  updated (last shipped milestone → v1l)
```

---

## Next: v1m — sandbox renderer integration

`SandboxLayer` mounts a hand-authored `sandbox.scene.toml`, cooks it through `crd-cooker`, and instantiates into the layer's `World`. The forward render path consumes the live World via the existing chunk visitor; `Transform.world` is read directly. v1m closes the loop authoring → render. After v1m: v1n reserved-slot freeze (Replication / ScriptComponent / Reflection trait acceptance test) closes Phase 3.0.
