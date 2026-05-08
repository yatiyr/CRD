# ADR-0058 — Öbek System: cooked entity-graph templates with composition, variation, and AAAA-tier future-proofing

**Status:** Accepted
**Date:** 2026-05-08
**Tags:** scene, ecs, cooker, resources, arch, renderer, networking, determinism

---

## Context

ADR-0055 shipped scene serialization (TOML → SCEN CRDR), and v1l shipped the scene cooker (`SceneCooker`). What's missing is the **reusable, instantiable entity-graph template** — what Unity calls a *prefab*, Unreal a *Blueprint*, Godot a *PackedScene*, Flecs a *prefab entity*, Bitsquid a *unit*.

Cerid does not adopt any of those names. The chosen term is **Öbek** (Turkish: cluster, group; from the verb *öbeklenmek*, "to gather into a cluster") — a native Turkic word with a precise technical meaning that aligns with the Cerid name's Anatolian etymology. *Öbek* is the engine's smallest reusable unit of authored entity content: a clustered set of entities + components + relations that gets packed by the cooker and pitched into a `World` at any anchor point.

This ADR locks the Öbek architecture: identity model, composition vs variation, override semantics, inherit policy (the per-component byte-sharing rule), eager-flatten-vs-lazy-reference cooking, and the format-level reservations that keep the engine future-proof for AAAA-tier features (GPU instancing, world partitioning, replication, replay, streaming) without ever breaking the cooked file format.

---

## Decision

### 1. Identity model — three layers

Every entity living inside an öbek has three coexisting identifiers:

| Identifier | Width | Stable across | Purpose |
|---|---|---|---|
| `file_idx` | u32 | cook re-runs (same source file) | local index inside the öbek; stable allocator (never reused even if entity removed) |
| `obek_root_id` | u64 | rename/restructure of source path (only changes when content hash changes) | identity of the source öbek; FNV-1a 64 of canonical path + content version |
| `instance_id` | EntityId | runtime-only | regular SlotMap id for the spawned entity |

The pair `(obek_root_id, file_idx)` is the **stable cross-session identity** of any entity-in-an-öbek. Hashed to a single u64 it becomes `ObekEntityGuid` — used by save files, network replication, replay, distributed authoring. Identity does not depend on entity name or position; renaming an entity in source preserves its `file_idx`.

### 2. Composition vs Variation — first-class distinction

Unity's clearest insight: **nested prefabs (composition)** and **prefab variants (inheritance)** are different operations. Cerid keeps them syntactically distinct, both first-class:

| Operation | TOML syntax | Cooker behavior |
|---|---|---|
| **Nested öbek** (composition) | `obek = "obek/wheel.obek.toml"` inside an entity entry | Spawns the referenced öbek as a subtree under that entity; child öbek retains its own `obek_root_id` for editor navigation |
| **Öbek variant** (inheritance) | `extends = "obek/vehicle_base.obek.toml"` at top of file | Inherits all entities/components/relations; restated fields override base |

Both operations chain unboundedly (variants of variants of variants; nested öbeks containing nested öbeks). Cycle detection at cook time. Variants composing nested öbeks works (a variant inherits the nesting of its parent + adds its own).

### 3. Override patches — typed first-class data

Override = a typed patch list applied at instantiation. **No string lookup at runtime.**

```cpp
struct ObekOverride
{
    u32  file_idx;          // entity (with name-fallback if file_idx unrecognised)
    u32  component_fourcc;  // 0 == relation override
    u32  field_path;        // packed: 16-bit offset + 8-bit element_idx + 8-bit kind
    u16  payload_size;
    u8   payload[];         // typed bytes, schema-validated at cook time
};
```

Patches:
- **Sortable** — canonically sorted (file_idx, component_fourcc, field_path) before apply → deterministic conflict resolution.
- **Diffable** — patch lists subtract / merge for editor "show me overrides on this instance."
- **Persistable** — patches serialise into SCEN (v1k) when an instance lives in a saved scene.
- **Symbolic-name-fallback** — if `file_idx` fails to validate (entity removed), look up by stored entity name; emit warning, don't crash.
- **Cook-time-validated** — patch list checked against the öbek schema manifest; invalid patches fail the cook (file_idx out of range, unknown component_fourcc, field_path doesn't match registered field, payload size mismatch).

### 4. Conflict resolution — explicit precedence stack

Unity's "sequential overwrite" leaves application order undefined. Cerid's stack is fixed:

```
Highest precedence
  Spawn-time override (passed to instantiate_obek)
    └── overrides
  Variant chain — deepest extends wins per field
    └── overrides
  Nested öbek's own values
    └── overrides
  Schema default
Lowest precedence
```

After instantiation, an instance is regular World data; runtime mutation (`world.set_X`) just overwrites whatever is there. There is no runtime "consult-the-prefab" indirection.

### 5. InheritPolicy — per-component byte-sharing rule

Stolen from Flecs's `(OnInstantiate, Inherit)` trait, refined for Cerid. Declared at component-registration time:

```cpp
w.register_component<Transform>  (InheritPolicy::Override);     // private copy per instance
w.register_component<MeshRef>    (InheritPolicy::Inherit);      // shared backing, transparent CoW
w.register_component<NetworkId>  (InheritPolicy::DontInherit);  // skipped on instantiation
```

| Policy | Memory at spawn | Read | Write | Use case |
|---|---|---|---|---|
| `Override` (default) | N × sizeof(T) | direct | direct | per-instance independent state (Transform, Velocity, custom fields) |
| `Inherit` | 1 × sizeof(T) (source) + N pointers | indirection | transparent CoW: backend intercepts first write per entity, copies to private storage, breaks the share for that entity only | shared static data across many instances (MeshRef, MaterialRef, animation skeleton, collision shape) |
| `DontInherit` | not present on instances | n/a | n/a | runtime-only state never authored (NetworkId, LoadState, ChangeDetectVersion, EditorSelectionFlag) |

**v1m ships all three policies fully implemented**, including transparent CoW for `Inherit`. Backend storage (ArchetypeChunkStorage and SparseSetStorage) gains a per-entity per-component "owned vs shared" flag bit; write paths check the bit, copy-on-write if shared. Eviction tracks shared backing via reference count.

### 6. Eager flatten by default; lazy reference opt-in

```toml
# Default — cooker walks extends + nested, emits one self-contained CRDR
[obek]
mode = "flatten"

# Opt-in for streaming — cooker emits OLNK chunk; loader resolves at instantiate time
[obek]
mode = "lazy"
```

- **Eager flatten** (v1m default) — zero runtime resolution cost; deterministic; ships immediately. Cost: cooked size = sum of all instances' bytes (no CRDR-level sharing within a single öbek).
- **Lazy reference** (Phase 3.5+ implementation) — smaller cooked footprint; live editing without recook of consumers; foundation for Cerid's eventual Level Instance / World Partition equivalent. Format reserves the `OLNK` chunk and an `OINF.flags` bit at v1m; runtime support lands when the streaming consumer ships.

### 7. Apply / Revert at four granularities

| API | Scope |
|---|---|
| `revert_field(instance, file_idx, comp, field_path)` | one field |
| `revert_component(instance, file_idx, comp)` | whole component |
| `revert_entity(instance, file_idx)` | whole entity in öbek |
| `revert_all(instance)` | all overrides |
| `apply_back_to_source(instance, override_subset, target_obek_path)` | push overrides into the source öbek file (re-cook) |
| `enumerate_overrides(instance) → ConstSpan<ObekOverride>` | for editor "override window" UI |

### 8. Per-instance ADD; soft-DELETE; no REORDER

Closing Unity's restrictions cleanly:

- **Add** — entities not in source: allowed; tagged `instance_only = true` on the spawned entity. Persist in saves alongside the override patch list.
- **Disable** — soft-delete source entities: allowed; tagged `disabled = true` on the instance. The entity still exists (file_idx stable, queries can opt to include disabled), but is excluded from default iteration.
- **Reorder** — forbidden by construction; file_idx is stable (allocator never reuses).
- **Restructure source** — explicit migration tool (`obekc migrate <old> <new>`) emits a patch script; editor previews diff before apply.

### 9. CRDR layout for cooked öbeks

```
type_fourcc = 'OBEK'

OINF — schema_version, entity_count, override_count, chain_depth, flags
       (mode bits: flatten | lazy; reserved 28 bits for streaming/replication/etc.)
OETB — per-entity record: { file_idx, name_offset, parent_file_idx, flags
                            (instance_only / disabled / streaming.lod / replication_mode bits) }
OCMP — per-component payload (SoA: file_idx[] + payload bytes); same shape as SCEN's
       per-component chunks; one OCMP-equivalent chunk per registered component
ORLS — relation records: { src_file_idx, tgt_file_idx, relation_fourcc }
OOVR — flattened override patches (variant chain pre-applied; chain preserved in OCHN)
OCHN — extends + nested öbek dependency list: canonical paths + content hashes (FNV-1a 64).
       Used by hot-reload watcher to detect upstream changes.
OLNK — (lazy mode only, Phase 3.5+) per-nested-öbek deferred reference: { ref_file_idx,
       child_obek_root_id, child_path, mount_point_file_idx }
OBAT — (reserved, Phase 3.5+) batch-instancing hint: { gpu_instanced, static_bake,
       lod_bucket }, populated when cooker detects an öbek tagged for batch spawning
```

CRDR sorts chunks by FourCC → deterministic byte order. Same source files + same overrides + same registration = bit-exact bytes (FNV-hash-verified, same pattern as v1l).

### 10. Cooker pipeline — riding the v1l SceneCooker substrate

```
.obek.toml  →  ObekCooker  →  OBEK CRDR

  Pass 1: parse extends chain (recursive; cycle detection)
  Pass 2: flatten variant chain (deepest wins per field)
  Pass 3: walk entities (document order); allocate file_idx (stable allocator)
  Pass 4: walk components (alphabetical key order — v1l determinism rule)
  Pass 5: resolve nested öbek references (recursive cook of dependencies)
  Pass 6: resolve relations (entity-name → file_idx via FNV map)
  Pass 7: resolve override patches; validate against schema; sort canonically
  Pass 8: bake world matrices (run TransformPropagation in temp World)
  Pass 9: emit CRDR — OINF + OETB + OCMP + ORLS + OOVR + OCHN [+ OLNK + OBAT]
```

~70% of the implementation generalises v1l's SceneCooker. **One cooker, two surface APIs** (`cook_scene`, `cook_obek`) sharing a kernel. Multi-error accumulation (advisor pin from v1l) is preserved.

### 11. Hot-reload — graph-aware

Cooker emits `OCHN` listing every transitive dependency. Watcher tracks chain mtimes/hashes. Any link change triggers transitive re-cook. Atomic swap; failed cook keeps last-good (matches shader/material hot-reload pattern). Reload event lists changed `file_idx`s so live instances can selectively re-apply.

### 12. Sub-instance API for nested öbeks

```cpp
auto inst = world.instantiate_obek(vehicle, ground_anchor);
// inst.entities maps file_idx → spawned EntityId for the WHOLE flattened tree.

for (auto& sub : inst.sub_instances())
{
    if (sub.source_obek_root == wheel_obek.root_id) { /* iterate wheels */ }
}
```

Sub-instances retain their `source_obek_root` so editor "find all wheels in this vehicle" + selective re-cook on `wheel.obek.toml` change both work.

### 13. Determinism contract

Same source files + same registration order + same overrides = bit-exact byte output across runs and machines. Verified by FNV-hashing two cook runs (same pattern as v1l). InheritPolicy::Inherit's CoW is ALSO deterministic — copy-on-write triggers at component-write time, not at spawn time, so spawn output is identical.

### 14. Break / Decompose / Variant / Compose / Modify — full operation matrix

| Operation | API / TOML |
|---|---|
| **Break (unpack)** — sever instance-to-source link | `world.unpack_obek(instance)` — all Inherit components forcibly unlinked; `ObekSourceLink` removed; overrides discarded |
| **Break keep overrides** — sever link, bake overrides into entities | `world.unpack_obek_keep_overrides(instance)` |
| **Decompose** — extract subgroup as new öbek file (tool) | `obekc extract <source> --root <name> --output <new> [--rewrite-source]` |
| **Variant** — create öbek extending another | `extends = "obek/vehicle_base.obek.toml"` + restated fields |
| **Compose** — embed nested öbek | `obek = "obek/wheel.obek.toml"` inside an entity entry + optional `overrides = [...]` |
| **Modify per-instance** | `instantiate_obek(obek, parent, ConstSpan<ObekOverride>)` |
| **Add per-instance entity** | `world.spawn_into_obek_instance(inst, "name") → EntityId` (tagged `instance_only`) |
| **Soft-delete per-instance entity** | override patch with `disabled = true` flag |

### 15. AAAA-tier future-proofing — five reserved hooks

The format reserves bits and the API reserves entry points now, so no Phase 3.5+ renderer / Phase 4.2 networking / Phase 8 replay work has to break öbek file format. They turn on bits already there.

#### 15a. Batch-instantiation API — entry point for GPU instanced rendering

```cpp
ObekBatchHandle batch = world.instantiate_obek_batch(
    forest_tree_obek,
    transforms,                                  // 10,000 world matrices
    parent_anchor,
    BatchHints{.gpu_instanced = true,
               .static_bake   = true,
               .lod_bucket    = 0});
```

The batch API:
- Allocates a contiguous range of `instance_id`s — GPU buffer indices stay packed.
- Tags spawned entities with `BatchInstanceTag{batch_handle, slot}` so renderer detects shared-draw eligibility.
- Pairs with `Inherit` policy on `MeshRef`/`MaterialRef` → 1 mesh + N transforms = 1 instanced draw call.

v1m ships the API + storage tags + format reservation (`OBAT` chunk). Renderer-side instanced draw path lands when the renderer needs it.

#### 15b. GPU-residency declared at component-registration time

```cpp
w.register_component<Transform>  (InheritPolicy::Override, GpuResident{.layout = std430});
w.register_component<MeshRef>    (InheritPolicy::Inherit,  GpuResident{.layout = std430,
                                                                       .static_bake = true});
w.register_component<MaterialRef>(InheritPolicy::Inherit,  GpuResident{});
```

The `GpuResident` index trait was reserved in ADR-0053 (Layer 5 reserved-slot freeze). Hooks into the component-event stream:

- Component added → schedule GPU upload to per-instance buffer.
- Component changed → invalidate range; renderer re-uploads.
- `static_bake = true` → upload once, never read CPU-side again. Foundation for read-only GPU buffers used by indirect draws.

v1m ships trait declaration + reserved API; upload backend lands in Phase 3.5+ paired with the renderer GPU-driven path.

#### 15c. Stable 64-bit GUID per entity-in-an-öbek

```cpp
struct ObekEntityGuid { u64 value; };
// = hash(obek_root_id, file_idx) — stable cross-machine, cross-session
```

Identity that survives serialization, replay, networking, distributed authoring. Enables:
- Replication: client + server agree which entity is which (Phase 4.2).
- Rollback netcode: snapshot N must match snapshot N across machines (Phase 4.2).
- Authoring multiplayer: two artists editing the same scene merge changes (Phase 7+).
- Replay: 1-hour replay file → fast-seek to event at minute 47 → identify the entity that owned that event (Phase 8).

Ships in v1m as a function on `ObekInstantiation`.

#### 15d. Per-component reservation flags — replication, static_bake, streaming

```cpp
struct ComponentRegistration
{
    InheritPolicy   inherit;
    GpuResident     gpu;
    ReplicationMode replication;        // None / Snapshot / Predicted (Phase 4.2)
    bool            static_bake;        // never changes after instantiation
    bool            streaming_per_lod;  // entity participates in LOD streaming
};
```

The OINF chunk reserves these bits now. v1m cookers populate them (most defaulted to 0). Phase 4.2 networking reads the `replication` bit; old öbeks just have `replication = None` → treated as not-replicated. **No format break, no migration needed.**

#### 15e. Streaming LOD tags per-entity

```toml
[entity.tree_distant]
streaming.lod    = 2                # only instantiated when LOD ≥ 2 active
streaming.region = "north_forest"   # spatial bucket for region-of-interest streaming
```

Reserved fields in the OETB entity table (8 bits LOD, 24 bits region-id pointing into a region string pool). The lazy-reference loader (pillar 6 second half, Phase 3.5+) reads these and selectively instantiates. UE5 World Partition's equivalent — but as ECS-component metadata rather than a separate world layer.

### 16. Animation / skeletal hierarchies

A skeleton fits cleanly as a nested öbek: skeleton root entity + one entity per bone, related via `ChildOf`/`AttachedTo`. Animation systems (Phase 3.2) consume the bone subtree; sockets (`AttachedTo`) hang weapons/props off named bones. No special-case "skeleton" type — pure ECS composition.

### 17. Cross-domain robustness

The öbek format is domain-agnostic:

- **Games** — characters, vehicles, weapons, levels.
- **Robotics** — URDF imports as nested öbeks (links + joints + collision shapes); deterministic snapshots for replay.
- **Aerospace** — orbital instruments as öbeks with `TransformF64` components (per ADR-0054 v1j debt; math layer ships the type, scene layer accepts custom serialize traits).
- **DAW** — synth tracks as öbeks (oscillator + filter + envelope + modulation graph all as components related by signal-flow relations).
- **Cinematic** — camera rigs, lighting setups, prop placements; öbek variants for "wide shot vs close-up" lighting tweaks.

### 18. Authoring schema — TOML

```toml
# obek/vehicle.obek.toml — full feature surface

extends = "obek/vehicle_base.obek.toml"     # variant chain (optional, depth unbounded)

[obek]
mode = "flatten"                            # "flatten" (default) | "lazy" (Phase 3.5+)

[entity.body]
Transform = { translation = [0, 0.5, 0] }
MeshRef   = "@asset:body.mesh"
MaterialRef = "@asset:body_paint.mat.toml"

[entity.wheel_fl]
obek = "obek/wheel.obek.toml"               # nested öbek reference (composition)
Transform = { translation = [1.0, 0, 1.5] }
overrides = [
    { file_idx = 0, component = "MaterialRef", field = "tint", value = [0, 0, 0, 1] }
]

[entity.spoiler]                             # NEW entity not in base — instance-only-add
MeshRef = "@asset:spoiler.mesh"
ChildOf = "body"
streaming.lod = 1                            # only instantiated at LOD ≥ 1

[entity.engine_block]
EngineSpec  = { hp = 450, redline_rpm = 8000 }   # variant override of base hp=200
```

### 19. Reserved API surface — frozen at v1m

```cpp
namespace crd::scene
{
class Obek;                                  // type
class ObekResource;                          // typed Resource payload
class ObekLoader;                            // ILoader for FourCC 'OBEK'
class ObekArtifactBuilder;                   // World → OBEK CRDR (test-only public)

struct ObekInstantiation;                    // move-only; file_idx → EntityId map + sub_instances
struct ObekOverride;                         // typed override patch
struct ObekBatchHandle;                      // returned by batch instantiation
struct ObekEntityGuid;                       // stable cross-machine 64-bit identity
struct ObekSourceLink;                       // marker component on instance roots

enum class InheritPolicy : u8 { Override, Inherit, DontInherit };

struct GpuResident { u32 layout; bool static_bake = false; };
struct ReplicationMode { /* None / Snapshot / Predicted — Phase 4.2 */ };

// World additions
EntityId            World::spawn_into_obek_instance(ObekInstantiation&, StringView);
ObekInstantiation   World::instantiate_obek(const Obek&, EntityId parent,
                                            ConstSpan<ObekOverride> = {});
ObekBatchHandle     World::instantiate_obek_batch(const Obek&, ConstSpan<Mat4f> transforms,
                                                  EntityId parent, BatchHints);
void                World::unpack_obek(ObekInstantiation&);
void                World::unpack_obek_keep_overrides(ObekInstantiation&);
void                World::revert_field(ObekInstantiation&, u32 file_idx, u32 fourcc,
                                        u32 field_path);
void                World::revert_component(ObekInstantiation&, u32 file_idx, u32 fourcc);
void                World::revert_entity(ObekInstantiation&, u32 file_idx);
void                World::revert_all(ObekInstantiation&);
ConstSpan<ObekOverride> World::enumerate_overrides(const ObekInstantiation&) const;
}
```

This API is frozen at v1m. New consumer phases (renderer, networking, replay, editor) implement against it; they do not change it.

---

## Comparison with elite engines

| Capability | Unity | Unreal | Godot | Flecs | **Cerid Öbek** |
|---|---|---|---|---|---|
| Variants + nested in one model | ✓ | partial | partial | — | **✓** |
| Multi-granularity Apply/Revert | ✓ | ✗ | ✗ | ✗ | **✓** |
| Per-component inherit policy | ✗ | ✗ | ✗ | ✓ | **✓ (declarative + CoW)** |
| Stable identity through restructure | ✗ | partial | ✗ | ✗ | **✓ (file_idx + name fallback)** |
| Deterministic byte output | ✗ | ✗ | ✗ | ✗ | **✓ (FNV-verified)** |
| Eager flatten + lazy opt-in | ✗ | ✓ (PLA only) | ✗ | ✗ | **✓ (both, format-reserved)** |
| Cross-domain (game/sim/DAW/cinematic) | ✗ | ✗ | partial | ✗ | **✓** |
| Compile-time-typed override patches | ✗ | ✗ | ✗ | ✗ | **✓ (no string lookup)** |
| Hot-reload graph-aware | partial | ✗ | partial | ✗ | **✓ (chain watcher)** |
| Stable GUID for replay/networking | ✗ | partial | ✗ | ✗ | **✓ (64-bit, cross-machine)** |
| Single TOML-cooker pipeline (prefabs + presets + scenes) | ✗ | ✗ | ✗ | ✗ | **✓** |

---

## Consequences

### Positive
- Authoring substrate that elite engines need; Cerid catches up to Unity prefabs + Unreal Level Instances + Flecs prefab traits in a single coherent system.
- Cross-domain (game / robotics / aerospace / DAW / cinematic) without per-domain bespoke types.
- Future-proof for AAAA features (GPU instancing, world partition, replication, replay) without format breaks; consumers wire up to reserved hooks as their phases land.
- Deterministic by construction — bit-exact bytes, replay/network-friendly.

### Negative / costs
- v1m is the largest single slice in Phase 3.0 (~6–8 days estimated). All three InheritPolicy values including transparent CoW + storage backend changes + format reservations.
- CoW write interception is the most complex part; needs careful ABA-safe + thread-safe design (current ECS is single-threaded per ADR-0052 v1h, but Phase 3.5+ parallel propagation will exercise the path).
- Format reservation hides complexity from v1m users but means future consumer-phase work must comply with the reserved bits / chunks; documented here as the contract.

### Open questions / debt

**Closed by v1m delivery (2026-05-08):**
- `Inherit-CoW` reference counting on shared backing — closed by v1m4b3. SharedComponentPool refcounts entries; instances release on destroy; pool entry frees when refcount → 0. ObekResource lifetime is the caller's responsibility (typical pattern: hold the ResourceHandle).

**Tracked in `docs/debt.md` § "Phase 3.0 v1m Öbek system" — three post-Phase-3.0 follow-ups:**
- **Hot-reload watcher with OCHN graph awareness** — format support shipped in v1m2 + v1m3; the watcher itself defers until filesystem-watching infrastructure lands.
- **`obekc extract` CLI tool** (pillar 14 "Decompose") — format support implicit via OBEK structure; the tool itself needs a new binary entry point under `tools/`.
- **InheritPolicy CoW dense-buffer optimization** — v1m4b's CoW wastes `sizeof(component)` bytes per shared dense slot; future optimization could allocate dense bytes lazily per-slot.

**Reserved (lifetime / API surface; revisit if a real consumer surfaces):**
- `ObekBatchHandle` lifecycle vs World destruction — needs definition; reserved as v1m+1 follow-up.
- Editor live-link mode (skip cooker, read .obek.toml directly) — Phase 7 (editor); format unchanged.

---

## References

- ADR-0049 — Entity / SlotMap (the L1 identity layer öbek instances live in)
- ADR-0050 — Storage backends (CoW Inherit touches both archetype + sparse-set)
- ADR-0051 — Relations as first-class (relations persist through öbek)
- ADR-0053 — Component-index framework (GpuResident trait reservation)
- ADR-0054 — Transform + propagation (cooker bakes world matrices in temp World)
- ADR-0055 — Scene serialization (SCEN format; Öbek shares the cooker substrate)
- ADR-0056 — Reserved API slots L6–L8 (Replication / Script / Reflection trait acceptance)
- v1l session log — `docs/sessions/2026-05-08-scene-v1l-cooker.md` (the SceneCooker pattern Öbek extends)

---

## Survey of elite-engine references

- Unity nested prefabs + variants — composition vs inheritance distinction.
- Unreal Level Instances + Packed Level Actors + ISM/HISM — hierarchical container instances; format-level ISM batching.
- Godot PackedScene local-to-scene resources — what NOT to do (auto-overrides created without intent; Cerid forbids implicit override creation).
- Flecs (OnInstantiate, Inherit) — declarative per-component inherit policy adopted into Cerid's `InheritPolicy`.
- O3DE Prefab Edit Mode — multi-granularity apply/revert UX.
- UE5 World Partition + sub-world partitions — streaming model; Cerid reserves the format hooks for the same in pillar 15e.
- Bevy `InheritFrom` — memory-pressure relief via shared inherited components; Cerid generalises via InheritPolicy::Inherit.
- Bitsquid Lua-table prefabs — programmable composition; Cerid achieves equivalent flexibility through TOML + cooker without runtime-language dependency.
