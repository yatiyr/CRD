---
id: ADR-0040
title: Cooker CLI + CMake integration
date: 2026-05-03
status: Accepted
tags: [resources, cooker, build, arch]
---

# ADR-0040 — Cooker CLI + CMake integration

## Context

ADR-0013 declared "Asset pipeline is a separate executable (`crd-tools/asset_cooker`). Runtime
never imports source assets." Phase 2.6 turns that one-line ADR into a real executable. The open
questions:

- How does CMake invoke the cooker — one big "cook everything" target, or one target per
  artifact?
- How are cook handlers registered — compile-time, DLL-loaded, scripted?
- How is incremental rebuild decided — file timestamps (broken across CI cache restores), input
  hashes, content hashes?
- How is determinism enforced — can two CI runs of the same source produce byte-identical
  packs?
- What third-party libraries can the cooker pull in that the engine cannot?

We had to pick answers that hold while the cooker grows from "one trivial blob handler" (v1b)
to "a full media-import pipeline with DLL plug-ins" (Phase 4+).

## Decision

**`tools/asset_cooker/` is a separate executable, never linked into engine binaries. Top-level
CMake `cook` target invokes it; `CRD_COOK_AS_TARGETS=ON` opts into per-artifact CMake targets.
Cook handlers register at compile-time in v1b; DLL-based registration arrives with Phase 4.
Determinism is structural (no timestamps in artifacts, content-hash incremental key,
`cook.log.toml` per run).**

### 1. `tools/asset_cooker/` is a separate executable

The cooker exe is built from `tools/asset_cooker/CMakeLists.txt` and depends on a TOOLS-only set
of modules:
- Engine modules it CAN link: `crd-core`, `crd-log`, `crd-memory`, `crd-containers`, `crd-config`,
  `crd-resources` (for the chunked-container reader/writer types and `ResourceId`).
- Engine modules it MUST NOT link: `crd-rhi`, `crd-rhi-vulkan`, `crd-renderer`, `crd-jobs`,
  `crd-platform-window` (the cooker is headless; it can use a stripped `crd-platform` for
  filesystem only).
- Tools-only third-party: `cgltf`, `stb_image` (or `DirectXTex` on Windows for BC compression),
  `dr_libs` (WAV/FLAC/MP3), `shaderc` + `spirv-reflect` for the GLSL handler.

The third-party tools list is INTENTIONALLY larger than the engine's. Heavy media-import code
that has no business at runtime stays in the cooker only.

### 2. CLI shape

```
asset_cooker  cook            --root <dir>  --out <pack.crdr>  [--target <profile>]
asset_cooker  manifest_dump   <pack.crdr>
asset_cooker  resource_dump   <pack.crdr>  <uuid>
```

Sub-commands are dispatched from `main.cpp`. Each lives in its own translation unit. `cook` is
the only sub-command that runs handler plug-ins; `manifest_dump` and `resource_dump` are
read-only inspectors on the chunked container reader from `crd-resources`.

`--target` selects a cook profile (`pc-quality`, `pc-shipping`, `mobile-low`, etc.) which the
cooker passes into each handler via `CookContext::profile`. Profiles are TOML files under
`tools/asset_cooker/profiles/`.

### 3. Top-level CMake `cook` target + opt-in per-artifact targets

```cmake
# Top-level (default)
add_custom_target(cook
    COMMAND $<TARGET_FILE:asset_cooker> cook
            --root ${CMAKE_SOURCE_DIR}/runtime/assets
            --out  ${CMAKE_BINARY_DIR}/cooked/pack.crdr
    DEPENDS asset_cooker
    USES_TERMINAL)

# Opt-in: one target per artifact (developer workflow)
option(CRD_COOK_AS_TARGETS "Emit one CMake target per cooked artifact" OFF)
```

Default mode: a single `cook` target. Suitable for CI and clean rebuilds.

`CRD_COOK_AS_TARGETS=ON` mode: the cooker is run once at CMake-configure time in a
`--list-artifacts` mode that prints one line per artifact. The CMake configure script consumes
this list and emits one `add_custom_command` per artifact with the source + `.meta` as
`DEPENDS`. This gives Ninja per-artifact incremental rebuild and IDE per-file build buttons,
at the cost of more CMake configure-time work.

### 4. Compile-time `register_cook_handler` in v1b — DLL-based with Phase 4

Cook handlers are functions with the signature `CookResult(const CookContext&)`. v1b registers
them via a free function called from `main.cpp` at startup:

```cpp
// tools/asset_cooker/src/main.cpp
register_cook_handler(".bin",       blob_passthrough_cook);
register_cook_handler(".glsl",      glsl_cook);
register_cook_handler(".mat.toml",  material_cook);
register_cook_handler(".png",       texture_cook);
// ...
```

This is the simplest and most-debuggable arrangement. The registry is a flat hash map keyed by
extension. Every handler is built into the cooker exe.

Phase 4 introduces DLL-based handler plug-ins (matching the C++ hot-reload scripting story from
ADR-0034). The signature stays the same; only the discovery mechanism changes (the cooker
scans a `plugins/` directory, `LoadLibrary`s each `.dll`, calls a known entry point that calls
`register_cook_handler`). v1b is forward-compatible because the registry is the API, not the
linker.

### 5. Incremental rebuild key

For each artifact the cooker computes:

```
key = blake3(cooker_version
           | loader_version
           | source_hash         ← blake3 of source bytes
           | options_hash        ← blake3 of canonicalized .meta options + cook profile
           | dep_graph_hash)     ← blake3 of (dep_id, dep_key) pairs for every transitive dep
```

The cached key (from the previous run) is stored next to the cooked artifact. On a re-cook,
matching key → reuse the existing artifact bytes verbatim, skip the handler invocation. Mismatch
→ re-run the handler.

This survives CI cache restores (timestamp-free), survives source moves (path is not part of
the key), and detects changes that only show up transitively (a shader edit cascades to every
material that uses it because the material's `dep_graph_hash` changes).

### 6. `cook.log.toml` per run — determinism audit

Every cook run writes `cook.log.toml` next to the output pack:

```toml
[run]
cooker_version = "1.0.0"
profile = "pc-shipping"
started_at = "2026-05-03T14:22:11Z"      # in the LOG, not in the pack
finished_at = "2026-05-03T14:22:43Z"

[[artifact]]
id = "0c8b9e2a-7f31-4e25-9c1d-2a4f7b9c1e88"
source = "shaders/sky.glsl"
type = "SHDR"
status = "recooked"   # or "skipped (incremental key match)" or "failed"
key = "8a3f...e21c"
input_size = 4823
output_size = 1289
elapsed_ms = 47

[[artifact]]
# ...
```

CI consumes this log to assert determinism: a second run of the same cooker over the same
sources must produce a `cook.log.toml` where every artifact is `"skipped"` AND a byte-identical
`pack.crdr` (verified by `blake3(pack.crdr)` compare).

### 7. Determinism rules for handlers (non-negotiable)

Any cook handler MUST:
- Produce byte-identical output for byte-identical input + identical options.
- Not read system clock, environment variables, or random sources.
- Sort containers before serialization (no relying on hash-table iteration order).
- Use only the allocator passed in `CookContext::allocator` (so cook-time memory accounting works).

A handler that violates these rules breaks CI determinism checks; the contract is enforced
socially (code review + CI) rather than mechanically (we can't sandbox handler I/O without
significant complexity).

### 8. Engine never links the cooker

`tools/asset_cooker/` is a sibling of `engine/` and `runtime/`. No engine target links any
cooker target. No engine target lists any cooker third-party dependency. CI verifies this by
building `engine/` with the cooker target excluded and confirming all engine tests pass.

This is the structural enforcement of ADR-0013: the engine literally cannot import a source
asset because the import code is not in its address space.

## Consequences

**Good:**
- The runtime never grows format-specific code. Adding glTF support to the cooker doesn't add a
  byte to the engine binary.
- Incremental cook works correctly across CI cache restores (no timestamp dependency).
- Determinism is verifiable: CI can hash the pack and assert it matches the previous run's hash.
- The cooker can pull in heavy third-party libraries (`shaderc`, `DirectXTex`) without
  inflating the engine.
- Per-artifact CMake targets are available for developer iteration without being mandatory in
  CI.
- DLL plug-in path is forward-compatible — the same `register_cook_handler` function works
  whether the call comes from the cooker exe's `main.cpp` or from a Phase 4 plug-in DLL.

**Constraints:**
- Cook handlers MUST be deterministic. The contract is non-negotiable; CI enforces it via hash
  compare.
- The cooker exe size grows with handler count. At Phase 4 the DLL-plug-in path makes this
  optional — DLC packs ship with their handler DLLs, the base cooker stays small.
- `CRD_COOK_AS_TARGETS=ON` runs the cooker at CMake configure time. On large asset trees this
  adds noticeable configure time. The default is OFF.
- Handlers cannot use `crd-jobs` (the cooker is single-threaded for v1b–v1g). Phase 4 adds
  parallelism; the handler signature already takes a `CookContext` by reference so adding a
  task scheduler later is non-breaking.
- Engine and cooker share `crd-resources` headers (the `CRDR` reader / writer + `ResourceId`).
  Changes to those headers force both to rebuild. Acceptable — they are the shared contract.

## References

- `docs/phases/phase-2.6-resources.md`
- ADR-0013 — Asset pipeline (separate cooker exe; the one-line ADR this fully fleshes out)
- ADR-0034 — C++ hot-reload DLL scripting (forward-compatible plug-in path)
- ADR-0036 — `crd-resources` module placement
- ADR-0038 — Cooked binary container format (the structural artifact format)
- BLAKE3 — fast cryptographic hash for the incremental key
