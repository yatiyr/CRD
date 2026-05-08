# 2026-05-07 — Phase 3.0 v1k: SceneResource + SceneLoader (persistence)

**Status at start:** Phase 3.0 v1j shipped. Transform + TransformPropagation. Scene tests 211 / 34716, six-config 727/727.

**Status at end:** v1k shipped — the **persistence layer**. SCEN artifact format with full World round-trip via `SceneArtifactBuilder` (build) + `SceneLoader` (parse) + `World::instantiate_scene` (restore). Forward-compat by FourCC; hard-fail on size/version mismatch. Determinism contract verified by bit-exact byte comparison. Six-config 741/741 / 738 release / 17 smokes. Scene tests 225 / 34783.

**This is the on-ramp for v1l (cooker handler), v1m (sandbox), Phase 4.2 (network snapshots), Phase 8 (replay).** A scene now travels: World → bytes → World identically.

---

## Goal of this session

Land Layer 4's persistence half (ADR-0055):
1. **`SceneResource`** — typed payload, parsed from a CRDR `'SCEN'` blob.
2. **`SceneLoader`** — `ILoader` registered for FourCC `'SCEN'` with `ResourceManager`.
3. **`SceneArtifactBuilder`** — emits SCEN bytes from a World snapshot. Test-only public API in v1k; v1l promotes it to the cooker handler.
4. **`World::instantiate_scene`** — restores entities/components/relations into a target World. Returns a move-only `SceneInstantiation` mapping file-local idx → live `EntityId`.
5. **Forward-compat persistence** — unknown FourCCs skip; known FourCC + size/version mismatch hard-fails.

## What shipped

### New module files

```
engine/scene/include/crd/scene/serialize.hpp       ~115 LOC — FourCC table + traits helpers
engine/scene/include/crd/scene/scene_resource.hpp  ~165 LOC — SceneResource, SceneLoader, SceneInstantiation, SceneArtifactBuilder
engine/scene/src/scene_resource.cpp                ~400 LOC — builder, loader, instantiate impl
tests/scene/test_scene_resource.cpp                ~440 LOC, 14 cases
```

### Modified

- `engine/scene/CMakeLists.txt` — added `crd-resources` to the public link list. crd-scene now depends on the resource framework for `ILoader` / `CrdrWriter`.
- `engine/scene/src/world.cpp` — `register_builtin_relations()` patched: each of the six built-in relations now also registers `relation_serialize_trait(kFourCC_Rel*)`. Persists by default through SCEN.
- `engine/scene/include/crd/scene/world.hpp` — new public method `World::instantiate_scene(const SceneResource&) → SceneInstantiation`. Documented forward-compat + hard-fail policy at the API doc-block.

### SCEN artifact format

CRDR container with `type_fourcc = 'SCEN'`. Six chunk types:

| Chunk | Layout |
|---|---|
| `INFO` | `SceneInfo` (16 B): schema_version + entity_count + component_count + relation_count |
| `STRP` | byte string pool (component / relation tag names; diagnostic only) |
| `CMPS` | `SceneComponentDescriptor[]`: per-component fourcc + version + name_offset + size + alignment + record_count + storage_hint |
| `ETBL` | `u32[]`: per-entity reserved-flags slot (advisor pin #8 — future flags: Pinned / Static / EditorOnly) |
| `C000`–`C0FF` | per-component payload (SoA): `u32 record_count + u32 reserved + u32 indices[] + (alignment pad) + u8 payloads[]` |
| `RELS` | `SceneRelationRecord[]`: per-relation src_idx + target_idx + relation_fourcc + reserved |

CRDR sorts chunks by FourCC at write time → deterministic file order. SCEN inherits this guarantee.

### Eight architectural decisions pinned

1. **Forward-compatible default with three reject conditions + one skip** (advisor pin #1):
   - Skip: unknown FourCC on entity → spawn entity anyway with known components.
   - Reject load (CRD_ASSERT in instantiate): known FourCC + size mismatch.
   - Reject load: known FourCC + alignment mismatch.
   - Reject load: known FourCC + version mismatch.

2. **FourCC primary + name diagnostic** (pin #2). FourCC is type identity; STRP names are for logs. Unknown-FourCC + matching-name does NOT silently rebind (would be an "I renamed but forgot to update FourCC" footgun).

3. **`SceneInstantiation` move-only** (pin #3). Same pattern as `Query`. Copy-deleted because the entity-id array would duplicate uselessly.

4. **`SceneArtifactBuilder` lives in `scene_resource.hpp`** (pin #4). Public-ish; v1l's cook_scene cooker promotes it to first-class without moving headers.

5. **Hard-fail on size/alignment mismatch** (pin #5). Silent best-effort would corrupt downstream state; loaders sit at the trust boundary.

6. **Determinism** (pin #6): walk order is registry-ascending for components, slot-iteration-order for entities, file-idx-order for relations. CRDR sorts chunks by FourCC. Same World → same SCEN bytes (verified by `test_scene_resource.cpp::"Determinism: identical world produces bit-exact SCEN bytes"`).

7. **Endianness: little-endian** (pin #7), inherits CRDR. Cross-platform big-endian is a v1n+ concern; documented.

8. **ETBL reserved-flags slot reserved now** (pin #8). Future flags: Pinned / Static / EditorOnly. Reserving the field at v1k = no schema-version bump when those land.

### `Transform` + 6 built-in relations get persistence for free

ChildOf, AttachedTo, Owns, Targets, DependsOn, PossessedBy each register with `relation_serialize_trait(kFourCC_Rel*)` at `register_builtin_relations()` time. Transform users add `transform_serialize_trait()` to their `register_component<Transform>()` call when they want SCEN persistence:

```cpp
w.register_component<Transform>(crd::scene::transform_serialize_trait());
w.register_builtin_relations();   // six relations all serialise out of the box
```

Components without `ComponentSerialize` trait (e.g. transient frame-scoped markers) are silently skipped on `build()`. This is the right shape — most user components are "live state" and don't need persistence.

## Bugs caught during integration

### Test #4 (hierarchy round-trip) — initial failure

The test built a parent + child hierarchy with `set_translation` on each, then serialised WITHOUT calling `step()`. Source's `Transform::world` was identity (no propagation had run). After load + `instantiate_scene`, target's child also had `world == identity`; `step()` on target propagated nothing because no entity carried `TransformDirtyFlag`.

Fix: test now calls `source.step(1.0/60.0)` BEFORE `build_scene` so propagation bakes the world matrix into the persisted bytes. Loading reproduces the baked world directly without requiring the target to mark every entity dirty.

This also documents the SCEN contract: **world matrices are baked into the persisted bytes**. If a caller wants to load a SCEN and have propagation re-derive world matrices from local TRS (because they changed local TRS post-load), they call `world.mark_transform_subtree_dirty(root)` per root.

### win-tidy: const_cast warning + identifier-naming

Three warnings:
- `const_cast<crd::u8*>(src)` to hand storage backend a mutable pointer — replaced with stack-staging buffer + memcpy. Also reduces UB risk for pedantic toolchains.
- Test file's `kFourCC_TestComponent` constant identifier didn't match the project's identifier-naming pattern (`k` + CamelCase). Renamed `kTestComponentFourCC` / `kOtherComponentFourCC`.
- Unused `using crd::scene::SceneInstantiation` declaration. Removed.

## Numbers

### Six-configuration green

| Config | Build | CTest |
|---|---|---|
| win-debug          | clean | 741 / 741 |
| win-relwithdebinfo | clean | 741 / 741 |
| win-release        | clean | 738 / 738 |
| win-asan           | clean | 741 / 741 |
| win-clang-cl       | clean | 741 / 741 |
| win-tidy           | clean | — (no v1k-introduced warnings) |

17/17 headless smokes per non-tidy config.

### Scene tests

- Pre-v1k: 211 cases / 34716 assertions.
- Post-v1k: 225 cases / 34783 assertions (+14 cases / +67 assertions).

### LOC

- `serialize.hpp`           ~115
- `scene_resource.hpp`      ~165
- `scene_resource.cpp`      ~410 (after const_cast cleanup)
- `world.hpp/cpp` deltas    ~50 (instantiate_scene + relation serialize traits)
- `test_scene_resource.cpp` ~440
- Total                     ~1180

## Deferred items pinned in `docs/debt.md`

1. **TOML-authored scene → SCEN cooking** is v1l (`cook_scene` cooker handler). v1k ships the artifact + loader; v1l ships the build-time pipeline that turns `.scene.toml` files into cooked SCEN packs.
2. **Streaming / incremental scene loading** — v1k loads-all-or-fail. Partial loads (e.g. "load only entities visible to camera") are a Phase 3.5+ slot for the streaming-LOD scenario.
3. **Schema migration** between SCEN versions — v1k pins `kSceneSchemaVersion = 1`, refuses mismatch. Migration tables (e.g., v1 → v2 transformer functions) are reserved for whenever we bump the version.
4. **Entity-name lookup** — finding a spawned entity by string name post-load. Out of v1k scope; user-defined `Name` component or query-by-component is the path. v1m sandbox might want explicit name lookup, addressed there.
5. **Per-instance component overrides** — prefab + override pattern. v1k loads scenes verbatim. v1m+ may layer overrides for inheriting from a base scene.
6. **Big-endian platform support** — v1k SCEN is little-endian. Cross-platform-byte-order swapping at load time is a v1n+ concern.
7. **Compressed SCEN chunks** — CRDR supports zstd-compressed chunks (bit 0 of chunk flags). v1k emits uncompressed. Compression support comes when SCEN files exceed practical sizes (multi-MB worlds); cooker (v1l) is the right place to enable it.
8. **`World::mark_all_transforms_dirty()` helper** — convenience for callers who load a SCEN with stale world matrices and want propagation to re-derive. Reserved if a use case appears.

## What this unlocks

v1l (cook_scene cooker handler) is the natural next slice. It consumes v1k directly:
- TOML parser reads `.scene.toml` files describing entities, components, relations.
- Builder calls `SceneArtifactBuilder::build` (now public-ish) to emit CRDR bytes.
- ResourceManager registers SCEN as a known resource type.
- Sandbox (v1m) does `auto handle = resources.load_sync<SceneResource>(scene_id); auto inst = world.instantiate_scene(*handle.get());`.

Beyond v1l:
- **v1m sandbox renderer integration**: load a cooked SCEN, instantiate, run TransformPropagation in PreRender, render via `query<Transform, Renderable>().skip_pending<Renderable>()`.
- **Phase 4.2 networking**: server snapshots are SCEN packs. Bit-exact determinism (v1k pin #6) means the same World produces the same snapshot regardless of when it's serialised.
- **Phase 8 replay**: every N frames, snapshot the World as a SCEN pack. Replay = walk the pack list and `instantiate_scene` each in order.
- **Robotics replay**: same as Phase 8. Recorded sensor readings + Transform snapshots replay deterministically.

## Commit message proposal

```
feat(scene): SceneResource + SceneLoader + SceneArtifactBuilder (v1k, ADR-0055)

Phase 3.0 v1k ships the persistence layer: a World's entity-component-
relation graph round-trips through a CRDR-formatted 'SCEN' container.

  - serialize.hpp: FourCC table + ComponentSerialize trait helpers
    (transform_serialize_trait, relation_serialize_trait,
    default_serialize_trait<T>(fourcc, version)).
  - SceneArtifactBuilder: walks World, emits CRDR bytes with INFO/STRP/
    CMPS/ETBL/C### /RELS chunks. Test-only public API in v1k; v1l's
    cook_scene cooker promotes.
  - SceneLoader (ILoader for FourCC 'SCEN'): validates schema version,
    parses chunks into ConstSpan views into the loaded bytes.
  - World::instantiate_scene(const SceneResource&) → SceneInstantiation:
    spawns entities, restores components by FourCC lookup, restores
    relations. Forward-compat: unknown FourCC silently skipped (counted
    in components_skipped / relations_skipped). Hard-fail on size/
    alignment/version mismatch.
  - Six built-in relations (ChildOf/AttachedTo/Owns/Targets/DependsOn/
    PossessedBy) auto-register with ComponentSerialize traits.

Determinism contract: same World → bit-exact same SCEN bytes (verified
in tests). Endianness: little-endian (inherits CRDR).

Eight architectural decisions documented in session log; eight follow-
ups pinned in docs/debt.md (TOML cooker → v1l, streaming loads → 3.5+,
schema migration → v1n+, name lookup → v1m, prefab overrides → v1m+,
big-endian → v1n+, compressed chunks → v1l, mark_all_transforms_dirty
helper → reserved).

Six-config DoD: 741/741 (was 727). 17/17 headless smokes per non-tidy
config. 225 scene tests / 34783 assertions (was 211 / 34716).
```
