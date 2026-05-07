---
id: ADR-0038
title: Cooked binary container format
date: 2026-05-03
status: Accepted
tags: [resources, arch, cooker]
---

# ADR-0038 — Cooked binary container format

## Context

ADR-0013 mandates that the runtime never sees source assets — only cooked binary artifacts.
Phase 2.6 needs a concrete on-disk format for those artifacts. Multiple resource types (shader,
material, texture, mesh, audio, scene, plus future custom DAW types) must coexist under one
runtime-side reader, otherwise every loader reinvents file I/O and the cooker becomes a special
case per type.

Considered options:

- **One file per type, hand-rolled headers.** Maximum flexibility, but every loader writes its
  own reader; manifest would need cross-file indexing.
- **Single tar-like archive without compression.** Simple, but no per-payload compression
  policy and no per-payload chunking.
- **TOML manifest + raw payloads alongside.** Human-readable manifest, but slow to load (parse
  TOML at startup), large on disk, and requires a second binary format for payloads anyway.
- **SQLite as the manifest.** Random-access friendly, but a heavy dependency, mutable file
  format that complicates determinism, and per-row overhead is large.
- **Chunked binary container shared by manifest AND per-resource artifacts.** One reader/writer
  for everything. The manifest itself becomes a "resource" with a special FourCC.

The fourth option matches the patterns proven in id Tech (`.pak`), Frostbite (`.toc/.cas`),
Unreal (`.uasset`/`.umap`) — chunked binary containers with typed sections. We adopt the same
shape with a Cerid-specific FourCC and a deliberately small header.

## Decision

**All cooked artifacts share a single chunked binary format. Magic `'CRDR'`. The runtime
manifest is itself a `CRDR` file with `type='PACK'`. Per-payload format inside chunks is
loader-defined; the framing is universal.**

### 1. Header — 32 bytes, fixed

```
+0   u32   magic       = 'CRDR'   ← 0x52, 0x44, 0x52, 0x43 on disk
+4   u16   version     = 1
+6   u16   flags       = (bit 0: any chunk uses zstd)
+8   u128  uuid                     ← ResourceId; for PACK files this is the pack id
+24  u32   type_fourcc              ← 'SHDR', 'MATR', 'AUDO', 'PACK', ...
+28  u32   chunk_count
```

All multi-byte fields little-endian. The 32-byte header keeps the body 16-byte-aligned for SIMD
loads. `version` is bumped on incompatible format change; loaders refuse mismatched versions.

### 2. Chunk — 24-byte header + payload

```
+0   u32   fourcc                   ← 'META', 'DEPS', 'BLOB', 'SPRV', 'REFL', ...
+4   u32   flags                    ← bit 0: compressed; bit 1: aligned-payload
+8   u64   uncompressed_size
+16  u64   compressed_size          ← == uncompressed_size if bit 0 clear
+24  u8[]  payload (compressed_size bytes; zero-padded to 16-byte alignment)
```

Chunk iteration is O(1) per chunk (size in header → skip to next). No central chunk table.
Random access by FourCC is a linear scan; chunk counts per artifact are small (typically 2–6).

### 3. Compression — zstd level 3, per-chunk opt-in

zstd level 3 is the default for chunks the cooker chooses to compress. Selection is
per-chunk, not per-file:

- `META`, `DEPS`, `STRP`, `MFST` — typically compressed (small text-like data).
- `BLOB` for already-compressed payloads (BC7 textures, FLAC audio) — stored uncompressed.
- `SPRV`, `REFL` — compressed (SPIR-V is verbose and compresses well).

Level 3 is the documented sweet spot for asset compression: ~3x ratio on typical engine data
at ~600 MB/s decompression, faster than disk on most targets. Higher levels save only single-
digit percent at multi-x cook-time cost.

### 4. Manifest = `type='PACK'` with three reserved chunks

The runtime manifest is one `CRDR` file. Reserved chunks:

- `MFST` — entry table. Each entry: `[ResourceId(16) | type_fourcc(4) | flags(4) |
  blob_offset(8) | blob_size(8) | name_strp_idx(4)]`. Sorted by `ResourceId`; binary search at
  load time.
- `STRP` — string pool. Length-prefixed UTF-8 strings. Holds debug names and source paths.
- `DEPS` — dependency edges: `[ResourceId src | u32 dep_count | ResourceId dep[]][]`. Used by
  the eviction policy to estimate cascading-evict cost (v1g) and by the cooker for the
  incremental rebuild key.

### 5. Reserved per-resource chunk FourCCs

The framework reserves a small base set; loaders may add private FourCCs.

| FourCC  | Meaning                                            |
|---------|----------------------------------------------------|
| `META`  | Small typed key/value blob; resource-level metadata|
| `DEPS`  | List of `ResourceId` this resource depends on      |
| `BLOB`  | Loader-defined opaque payload                      |
| `SPRV`  | SPIR-V module (shader loader)                      |
| `REFL`  | Reflection metadata (shader loader)                |

Unknown FourCCs are skipped by the framework reader; loaders reading their own private FourCCs
get them via the iteration API.

### 6. Determinism rules

- No timestamps inside chunks. The cooker writes `cook.log.toml` separately for audit;
  the artifact bytes are timestamp-free.
- Chunks within an artifact are sorted by FourCC at write time. This makes byte-equal
  artifacts diffable.
- Padding bytes are zero-filled (not "uninitialized memory may contain anything").
- zstd is invoked with `ZSTD_c_contentSizeFlag = 0` and a fixed parameter set so the same input
  produces the same output across runs and across zstd versions within the supported range.

### 7. `manifest_dump` CLI sub-command — required from v1a

The cooker exe ships with a `manifest_dump <pack.crdr>` sub-command from day one. It prints:
- Pack header (magic, version, flags, pack id).
- Each `MFST` entry: id, type FourCC, offset, size, name (from `STRP`).
- `DEPS` edges in `id → [id, id, ...]` form.

This is the debug interface for "what's in this pack" without needing a hex editor. A
`resource_dump <pack.crdr> <uuid>` companion sub-command prints the chunks of one entry.

### 8. Per-payload format inside `BLOB` — resolved by ADR-0055

The original ADR deferred this to a "Phase 3.1c" slice. With the Phase 3.0 architecture
locked (ADRs 0049–0057), the question is closed by ADR-0055: scene data uses **hand-rolled
POD layouts inside a CRDR `SCEN` artifact** — no FlatBuffers, no Cap'n Proto. Reasoning:
single language, single platform, controlled writer + reader, smallest mmap-friendly layout
wins. Per-loader payloads remain hand-rolled POD across the board (shader, material, mesh,
texture, scene), with explicit `version` fields and registered migration callbacks.

## Consequences

**Good:**
- Single reader / writer infrastructure handles every resource type, the manifest, and any
  future loader.
- `manifest_dump` works for any pack regardless of contained loader types.
- Determinism is structural (byte-equal output for byte-equal input) — CI can verify pack
  reproducibility with a hash compare.
- zstd compression is per-chunk: already-compressed payloads aren't re-compressed wastefully.
- Fits in two registers' worth of header inspection per artifact open; trivial to mmap.

**Constraints:**
- Every loader must agree on the chunk-iteration contract (read header → enumerate chunks →
  consume known FourCCs → ignore unknown). Loaders that try to seek to a fixed offset will
  break.
- 32-byte header + 24-byte per-chunk overhead is fine for typical asset sizes (hundreds of
  bytes minimum) but wasteful for sub-100-byte resources. Such resources are uncommon in
  practice; if they appear we'd consider a "tiny artifact" format under a separate magic.
- The `MFST` linear-scan / binary-search lookup is O(log n) per resource. At hundreds of
  thousands of entries we may need a paged or sharded manifest — see open question in
  `phase-2.6-resources.md`.
- Hand-rolled per-loader `BLOB` schemas mean each loader pays a small evolution cost when its
  format changes (versioning is loader-private). Closed permanently by ADR-0055 — POD blobs
  are the engine-wide policy for cooked artifacts.

## References

- `docs/phases/phase-2.6-resources.md`
- ADR-0013 — Asset pipeline (separate cooker exe; runtime never sees sources)
- ADR-0055 — Scene serialization (closes the per-payload-format deferral with `SCEN` CRDR)
- ADR-0036 — `crd-resources` module placement + loader-registry pattern
- ADR-0040 — Cooker CLI + CMake integration
- id Tech `.pak`, Frostbite `.toc`/`.cas`, Unreal `.uasset` — prior-art chunked container patterns
- zstd — Facebook compression library (BSD)
