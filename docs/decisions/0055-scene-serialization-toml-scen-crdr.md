# ADR-0055 — Scene serialization: TOML authoring + SCEN CRDR cooked

**Status:** Accepted
**Date:** 2026-05-06
**Tags:** scene, ecs, resources, cooker, arch

---

## Context

ADR-0020 committed to "TOML for authoring, cooked binary for runtime" but deferred the binary-format choice (FlatBuffers vs Cap'n Proto) to "the Phase 3.1c slice." With the eight-layer architecture (`docs/phases/phase-3.0-scene-ecs.md`) now locked, the format choice cannot defer further — `crd-scene` v1m needs a concrete cooked layout.

This ADR closes that deferral and locks the authoring schema, the cooked artifact format, the asset reference resolution, and the component-type registry that drives both the cooker and the runtime loader.

---

## Decision

### 1. Author scenes in TOML; cook to a CRDR `SCEN` artifact

**Both FlatBuffers and Cap'n Proto are rejected.** Those formats are designed for network protocols and cross-language interop — neither of which describes our use case. We load a cooked scene once (or on hot-reload), run it on a single platform/language, and want the smallest tightest binary representation possible.

Cooked format: a new `SCEN` FourCC artifact in the existing CRDR container (ADR-0038). Reuses CRDR's chunking, zstd compression, manifest, hot-reload, and mounting machinery. No new container code, no new file format.

```
SCEN artifact layout:
  CRDR header (magic, version, FourCC = 'SCEN')
  ETBL chunk — entity table (count, parent links by index, name string offsets)
  STRP chunk — string pool (deduplicated entity names)
  CMPS chunk — component schema (component type FourCCs in registration order)
  C### chunks — one per component type, dense SoA blob (FourCC = component's FourCC)
  RELS chunk — relations (target-entity references, indexed by relation tag)
```

### 2. TOML authoring schema

Flat list of entities. Components nested under each entity. Hierarchy expressed by `parent = "name"`:

```toml
# my_scene.scene.toml
[scene]
name = "demo_room"

[[entity]]
name = "world_root"

[[entity]]
name = "spinning_cube"
parent = "world_root"

[entity.components.transform]
translation = [0.0, 1.0, 0.0]
rotation    = [0.0, 0.0, 0.0, 1.0]
scale       = [1.0, 1.0, 1.0]

[entity.components.renderable]
mesh     = "@asset:meshes/cube.glb"
material = "@asset:materials/default_lit.mat.toml"

[[entity]]
name = "scene_light"
parent = "world_root"

[entity.components.transform]
translation = [2.0, 4.0, 1.0]

[entity.components.directional_light]
color     = [1.0, 0.95, 0.85]
intensity = 5.0
```

- `[[entity]]` — array-of-tables: each table is one entity.
- `name` is required per entity. Used by `parent =` references and for editor display.
- `parent` is optional. Absent = scene root.
- `[entity.components.<snake_name>]` — component data, keyed by snake_case name registered with the component system.
- Component fields are recursively decoded by the component's registered TOML deserializer.

### 3. Asset reference resolution: `@asset:<path>` syntax

External asset references (mesh, texture, material, shader) appear as strings prefixed with `@asset:`. The cooker resolves these to `ResourceId` UUIDs at cook time:

```toml
mesh     = "@asset:meshes/cube.glb"          # cooker reads .meta sidecar; replaces with UUID
material = "@asset:materials/default_lit.mat.toml"
```

Cook process:
1. Parse TOML.
2. For each `@asset:<path>` reference, locate `<path>.meta` sidecar.
3. Read UUID from sidecar.
4. Emit UUID into the SCEN binary; the string never appears in the cooked artifact.
5. Add the UUID to the `dependencies` list of the SCEN's `ManifestEntry` (already supported by CRDR — see ADR-0038).

If a `.meta` is missing, cook fails with a precise error: `"my_scene.scene.toml line 14: cannot resolve @asset:meshes/cube.glb — meta sidecar not found"`.

This means **runtime loading never parses paths or strings** — the loaded scene already has resolved `ResourceId`s ready to feed `ResourceManager::load_async<MeshResource>`.

### 4. Component-type registry

Components register their TOML schema and binary layout with the component system:

```cpp
world.register_component<Transform>(
    StorageHint::Archetype,
    History{8},
    ComponentSerialize{
        .name        = "transform",
        .fourcc      = make_fourcc('T','R','N','S'),
        .version     = 1,
        .deserialize_toml = &Transform::deserialize_toml,    // toml -> binary blob
        .serialize_toml   = &Transform::serialize_toml,      // binary blob -> toml (editor)
        .read_blob        = &Transform::read_blob,           // blob -> in-memory T
        .write_blob       = &Transform::write_blob,          // in-memory T -> blob
    }
);
```

The `ComponentSerialize` trait is opt-in. Components without it (e.g. transient runtime-only state) cannot be serialized; the cooker errors if a scene references one.

The cooker walks the component registry to:
- Resolve TOML keys (`[entity.components.transform]` → Transform's deserializer).
- Emit one `C###` chunk per component type that appears in the scene, packed SoA.
- Emit the `CMPS` schema chunk listing FourCCs and versions of all referenced components.

The runtime loader walks the schema chunk to validate compatibility, then iterates entity rows and dispatches each component blob to its registered `read_blob` function.

### 5. Versioning and migration

Each component's `ComponentSerialize::version` is stored in the `CMPS` schema chunk. On load:
- Version match → fast path, direct blob copy.
- Version older → call registered migration function, if any. Else fail-load.
- Version newer → fail-load (cooked artifact requires newer engine).

Failed migrations log a clear diagnostic and the entity is skipped rather than crashing the load. Editor tools recook from TOML to upgrade.

### 6. Relations in SCEN

The `RELS` chunk encodes:

```
RELS chunk:
  count: u32
  for each relation:
    tag_fourcc: u32        # Relation tag identifier (ChildOf -> 'CHLD', etc.)
    src_entity: u32        # index into ETBL
    target_entity: u32     # index into ETBL
```

Relations are decoupled from entities: an entity in ETBL doesn't carry its own ChildOf — it appears as the `src_entity` of a RELS entry. This matches the runtime data model where relations are components, not entity-table fields.

The TOML `parent = "name"` shorthand expands at cook time into a `Relation<ChildOf>` entry. Other relations are authored explicitly:

```toml
[[entity.relations]]
tag = "AttachedTo"
target = "weapon_socket"

[[entity.relations]]
tag = "Targets"
target = "enemy_drone"
```

### 7. Hot-reload

The CRDR mount machinery already supports mtime-watch hot-reload (Phase 2.6 v1f). When `my_scene.scene.toml` is recooked, the SCEN artifact bumps its mtime; `ResourceManager::poll_hot_reload` picks it up; the scene loader re-builds the world.

For the editor (Phase 7), live-edit will use a finer-grained reload — patch only changed entities/components rather than rebuilding the whole world. That is editor-specific and not part of this ADR.

---

## Rationale

### Why CRDR over FlatBuffers / Cap'n Proto

Both FlatBuffers and Cap'n Proto are designed for:
- Cross-language interop (we have one language).
- Network wire format (we don't ship scenes over the network — networking sends component diffs, not whole scenes; ADR-0035).
- Forward compatibility with unknown fields (we control both writer and reader; explicit version + migration is cleaner).
- Schema evolution at scale (we have a manageable component count).

What they cost:
- ~50 KB of header per artifact.
- Pointer chasing on read (FlatBuffers offset tables) or copy on read (Cap'n Proto Reader→Builder).
- A schema language we'd have to keep in sync with C++ component definitions.

CRDR + per-component blobs is smaller (zstd-compressed POD blobs are ~30% smaller than equivalent FlatBuffers), faster (mmap-friendly, no offset traversal), and reuses infrastructure we already ship.

### Why component-type registry over reflection

Reflection (Phase 7, ADR-0056 reserves the slot) is general-purpose introspection. The cooker doesn't need full reflection — it needs serialization. Splitting the two means:
- Components serializable for scenes ship in Phase 3.0.
- Reflection (which adds field-level inspector access for the editor) ships in Phase 7 without forcing the editor's complexity onto the cooker.
- Components can opt out of serialization without losing reflection (an editor-only debug component is inspectable but not saveable).

### Why explicit `@asset:` syntax

Implicit "any string that looks like a path is an asset reference" is fragile and produces obscure errors. `@asset:` is unambiguous, machine-readable, and the editor tooling can offer auto-complete from the asset browser. Future prefixes (`@entity:` for cross-scene entity refs, `@expr:` for procedural defaults) slot in without ambiguity.

---

## Consequences

- `crd-scene` ships in Phase 3.0 with the SCEN cooked format and TOML authoring.
- The asset cooker gains a `.scene.toml` handler in v1m.
- `ResourceManager` gains `SceneLoader` registered for FourCC `'SCEN'`.
- Component-serialize traits are required for any component that should appear in saved scenes. Components without them are runtime-only.
- The "FlatBuffers vs Cap'n Proto" deferral in ADR-0020 is closed by this ADR.
- Hot-reload of scenes uses existing CRDR mtime-watch machinery; no new subsystem.
- Cooked scenes are mountable in the same packs as other assets (`crd-sandbox` pack will eventually contain the demo `.scene.toml` cooked alongside meshes and textures).

---

## References

- ADR-0020 — Scene & ECS hybrid (closes "binary format choice" deferral)
- ADR-0036 — `crd-resources` module + loader registry (SceneLoader follows this pattern)
- ADR-0038 — Cooked binary container format (CRDR base)
- ADR-0039 — `ResourceHandle<T>` semantics
- ADR-0040 — Cooker CLI + CMake integration
- ADR-0049 — Entity identity (entity table format)
- ADR-0051 — Relations (RELS chunk format)
- ADR-0053 — Component index slot framework (ComponentSerialize trait registration)
