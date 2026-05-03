---
id: ADR-0037
title: ResourceId hybrid UUID scheme
date: 2026-05-03
status: Accepted
tags: [resources, arch]
---

# ADR-0037 — ResourceId hybrid UUID scheme

## Context

`crd-resources` needs a global identity scheme for assets. The candidates were:

- **Path-based** (`"shaders/sky/atmosphere.glsl"`) — simple, but renaming a file breaks every
  reference. Unacceptable for editor workflows.
- **Hash-of-content** alone — content-addressed, naturally dedup-friendly, but two cooks of the
  same source with different cooker options produce different ids; updating an asset
  intentionally also breaks every reference.
- **Sequential integer** (Unreal-style FName + integer table) — fast, compact, but fragile across
  branches/merges in VCS and impossible to mint at runtime for procedural resources.
- **128-bit UUID** (Unity / Godot) — globally unique, mintable anywhere, stable across renames.
  The pattern proven across major commercial editors.

Cerid's multi-domain target makes the requirements stricter than any single editor:

- **Games** need rename stability — designers reorganise asset trees constantly.
- **Simulation / robotics** need to mint identifiers at runtime for procedurally-generated
  resources (a freshly-baked light probe, a captured sensor frame stored as a `crd-resources`
  blob for replay).
- **DAW projects** need project-relative identity — a `.daw_project.toml` references samples
  by id, and that id must survive moving the entire project directory between machines.
- **Content-addressed dedup** is still useful — two artists importing the same texture should
  produce the same cooked artifact bytes (saving disk and download time).

## Decision

**`ResourceId` is a 128-bit value with three minting modes. The runtime authority is the cooked
manifest; `.meta` sidecars hold the source-tree authority.**

### 1. Layout — `{ u64 hi; u64 lo; }`

128 bits as two `u64` fields, little-endian. Wide enough to make collision astronomically
improbable; narrow enough to fit in two registers and pass by value cheaply. The textual form is
the canonical UUID 8-4-4-4-12 hex with hyphens.

### 2. Three minting modes

**Mode A — `mint_random()` (UUID v4).** A cryptographically-random 128-bit value. Used by:
- The cooker at first import — the chosen id is written to `<source>.meta` and never changes.
- Application code at runtime for genuinely procedural resources (a baked light probe, an audio
  render-bounce, a captured sensor frame).

**Mode B — `from_content(bytes)` (UUID v5).** SHA-1 of `bytes` under a fixed Cerid namespace,
truncated to 128 bits and tagged as v5 per RFC 4122. Used by the cooker for content-addressed
dedup: two source files with byte-identical contents produce byte-identical cooked artifacts and
share one id. Loaders in tests use this when they want stable ids across runs without manual id
management.

**Mode C — `parse(text)`.** Round-trip from textual form. Used by `manifest_dump`, command-line
tools, hand-edited `.meta` files, and serialized debug logs.

### 3. `.meta` sidecars are the SOURCE-tree authority; cooked manifest is the RUNTIME authority

Each source asset has an adjacent `<asset>.meta` TOML sidecar (Unity / Godot pattern):

```toml
# textures/wood.png.meta
[id]
uuid = "0c8b9e2a-7f31-4e25-9c1d-2a4f7b9c1e88"

[import]
imported_at = "2026-05-03T14:22:11Z"
cooker_version = "1.0.0"
loader_version = "texture:1"

[options]
format = "BC7"
mips = "auto"
```

`.meta` files travel with the source in VCS. They are NEVER shipped — the cooked manifest is the
runtime authority. At cook time the cooker reads `.meta` to get the id; if no `.meta` exists it
mints one and writes it. Renames of the source file leave the id untouched (only the
`source_path` debug field in the manifest changes).

### 4. `kNullResourceId = {0, 0}` is the sole reserved value

`ResourceId{0, 0}` is the explicit "null" id. `is_null()` checks for it. `mint_random()` is
guaranteed never to return it (the v4 generator rejects all-zero output). `from_content()` uses
the v5 algorithm which sets the version+variant bits, so it cannot produce all-zero either.

### 5. Equality is bitwise; hashing is the low 64 bits

`ResourceId::operator==` is the default member-wise compare. Hash maps key on `lo` (already
random for v4 ids and well-distributed for v5 ids; using both halves wastes cycles for no
collision-rate benefit at realistic table sizes).

## Consequences

**Good:**
- Renames in the source tree cost nothing — the id sits in `.meta`, not in the path.
- Procedural runtime resources (digital-twin captures, baked probes, audio render-bounces) get
  globally-unique ids without coordinating with the cooker.
- DAW projects move between machines without reference breakage — the project's `.meta` files
  travel inside the project directory.
- Content-addressed dedup falls out of `from_content()` — the cooker can choose to use it for
  large-asset families (texture atlases, environment maps).
- The textual form is debuggable: a UUID in a log message identifies a unique resource and
  copies cleanly to `manifest_dump`.

**Constraints:**
- `.meta` files MUST be committed to VCS alongside their sources. Forgetting one means the
  cooker re-mints the id, breaking every reference. Tooling will warn at cook time when a
  source has no `.meta` and create one (the warning is loud enough to catch missed commits in
  code review).
- 128-bit ids cost 16 bytes per reference. Compared to a 32-bit handle this is 4x. Accepted —
  asset references are not in inner loops; rendering hot paths use lower-level handles
  (`PipelineHandle`, `ImageHandle`) that are already 32-bit.
- Two cooked packs from independent teams CAN produce id collisions across `mint_random()`
  outputs (probability ~ 2^-122, but two third-party packs intentionally sharing an id is
  possible). Resolved by the multi-mount last-wins rule plus a `Warn`-level log, not by
  rejecting the collision.
- The runtime cannot enumerate sources from ids alone — the manifest stores `source_path` only
  as a debug field. Production builds may strip it; debug builds keep it for `manifest_dump`.

## References

- `docs/phases/phase-2.6-resources.md`
- ADR-0036 — `crd-resources` module placement + loader-registry pattern
- ADR-0038 — Cooked binary container format (where ids live in the MFST chunk)
- RFC 4122 — UUID specification (v4 random, v5 SHA-1 namespaced)
- Unity Asset Database — `.meta` sidecar pattern
- Godot Engine — `.import` sidecar pattern (same idea, different syntax)
