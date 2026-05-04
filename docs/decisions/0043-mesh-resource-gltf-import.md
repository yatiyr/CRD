# ADR-0043 — MeshResource vertex layout + glTF import scope

**Status:** Accepted  
**Phase:** 2.7 v1b  
**Tags:** `[resources]` `[renderer]` `[cooker]`

---

## Context

Phase 2.7 introduces `MeshResource` as the geometry asset type. Two design decisions are
entangled: the interleaved vertex layout written by the cooker and read by the loader, and
which subset of glTF 2.0 the cooker handles.

### Vertex layout options

| Option | Bytes/vert | Pros | Cons |
|--------|-----------|------|------|
| Position only | 12 | Trivial; useful for shadow/depth passes. | Not a real mesh layout; materials need at least UVs. |
| Pos + Normal + UV0 | 32 | Compact; covers basic lit material. | No tangents → no normal mapping (a core PBR feature). |
| Pos + Normal + UV0 + Tangent | 48 | Full lit material + normal mapping. Standard for PBR. | 48 bytes; fine for v1. |
| Split streams (Pos VBO, Attrib VBO) | — | Depth prepass only reads pos stream, saves bandwidth. | Two buffers; more complex `DrawItem`; premature. |

### glTF import scope options

| Option | What it handles | Complexity |
|--------|----------------|------------|
| Minimal (triangle lists, one primitive) | Static mesh + single material | Low |
| Standard (all primitive types, multi-material) | Real asset pipeline | Medium |
| Full (skinned, morph targets, animations, cameras) | Game-ready | High |

---

## Decision

**Vertex layout:** Interleaved, 48 bytes per vertex:

```
bytes  0–11: float3  position
bytes 12–23: float3  normal
bytes 24–31: float2  uv0
bytes 32–47: float4  tangent  (xyz = tangent direction, w = bitangent sign: +1 or -1)
```

All four attributes are always present. If a glTF mesh lacks `TANGENT`, the cooker generates
tangents using MikkTSpace (`mikktspace.h`, vendored single-header). If a mesh lacks `TEXCOORD_0`,
UVs are zero-filled. If a mesh lacks `NORMAL`, normals are generated via cross-product (flat normals).

Index buffer: always `u32` in the artifact. The cooker upcasts `u8` and `u16` glTF index buffers.

**glTF import scope (v1b):**
- Triangle list primitives only (`TRIANGLES` mode). Other modes (strips, fans, points, lines) are
  skipped with a cook warning.
- Static meshes only. `SKIN` and animation channels are silently ignored (not an error — the mesh
  geometry still cooks cleanly).
- One MESH artifact per glTF mesh (not per scene node, not per file). A glTF file with `n` named
  meshes produces `n` MESH artifacts. UUIDs come from `.meta` sidecars adjacent to the `.glb`/`.gltf`
  file, keyed by mesh name: `<file>.mesh.<mesh_name>.meta`.
- Materials: each primitive records the `ResourceId` of the associated MATR artifact, looked up
  from the material's `.mat.toml.meta` sidecar by matching the glTF material name. If no match,
  the primitive stores a null UUID.
- `.glb` (binary glTF) and `.gltf` + `.bin` (text glTF with external buffer) both supported.
  Embedded base64 buffers in `.gltf` are supported via cgltf.

**Why interleaved (not split streams):**
The depth-prepass bandwidth argument for split streams (ADR-0043 alternative) is real but
premature. The prerequisite is a per-variant pipeline cache keyed on vertex stream subset, which
is a Phase 3.2 concern. Adding split stream support now creates complexity with no current
consumer. The 48-byte interleaved layout is consistent with what most real-time engines use for
PBR geometry and is a clean upgrade path:
- When split streams land (Phase 3.2), the cooker emits two `VERT` chunks tagged by stream type.
- The loader detects which chunks are present and populates the right buffer.
- Old single-stream artifacts continue to load correctly (only one `VERT` chunk = no split).

---

## Consequences

- `MeshResource` holds three `Array<u8>` members: `vertices`, `indices`, and a `primitives`
  array of `MeshPrimitive` structs (vertex/index byte offsets + counts + material UUID).
- The interleaved vertex stride (48) is a compile-time constant in the cooker and loader.
  If the layout ever changes, bump `kMeshLoaderVersion`.
- `mikktspace.h` is vendored into `tools/asset_cooker/` (single-header, permissive license).
- `cgltf.h` is vendored into `tools/asset_cooker/` (single-header, MIT license). It was already
  declared as a cooker dep in Phase 2.6; v1b wires it.
- The glTF scope decision (no animations, no skins) means `MeshResource` has no bones array.
  Skeletal animation (Phase 3.2) will introduce `SkinnedMeshResource` as a separate type with
  its own FourCC — it does not extend `MeshResource`.
- GPU upload (`GpuMeshUploader`) allocates one vertex `Buffer` and one index `Buffer` per
  `MeshResource`. Primitive sub-ranges are byte-offset slices into those two buffers.

---

## CRDR artifact layout (`type='MESH'`)

```
VERT chunk: raw vertex bytes (vertex_count × 48 bytes)
INDX chunk: raw index bytes (index_count × 4 bytes, u32)
PRIM chunk: primitive table
  +0  u32  primitive_count
  per primitive (32 bytes):
    +0  u32  vertex_count
    +4  u32  index_count
    +8  u32  vertex_byte_offset
    +12 u32  index_byte_offset
    +16 u8[16] material_id  (ResourceId hi+lo LE; all-zero = no material)
```

FourCCs: `kFourCC_MESH`, `kFourCC_VERT`, `kFourCC_INDX`, `kFourCC_PRIM`.

---

## Alternatives not taken

- **GLTF runtime loading (no cooker):** Parse glTF at runtime. Rejected per ADR-0013 — runtime
  never sees source assets. Cooker always pre-processes into the CRDR binary format.
- **One artifact per glTF file:** Simpler sidecar story (one UUID per file). Rejected because a
  single glTF file frequently contains multiple named meshes that the scene system needs to
  reference independently. Per-mesh artifacts align with how `ResourceId` grants stable identity
  to individually referenceable assets.
- **Assimp instead of cgltf:** Assimp handles many formats but is a heavy dependency (300 kLOC,
  complex CMake). cgltf is a single-header library covering exactly glTF 2.0 — our primary source
  format. Other formats (FBX, OBJ) can be added as separate cooker handlers when needed.
- **Basis Universal mesh format:** Not a thing — Basis is a texture compression standard.
  Mesh format is binary CRDR; no external schema needed.
