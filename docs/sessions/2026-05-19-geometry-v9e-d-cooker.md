# Session 2026-05-19 — geometry-v9e-d cooker

## Summary

Shipped Phase 3.1.7 v9e-d: the shader-helpers cooker. Closes the
`designer authors → cooker emits → renderer loads` loop for the
`crd-geometry-shader-helpers` module. The cooker writes the shared
GLSL/HLSL prelude (every `sd_*` + smin/op/domain helper) and per-IR named
SDF functions as `.glsl` + `.hlsl` files on disk. Downstream renderers
consume the cooked text directly — no link-time dependency on the cooker
library.

## What landed

### Public API (`engine/geometry-shader-helpers/include/crd/geometry/shader_helpers/cooker.hpp`)

```cpp
struct CookResult
{
    bool                                              ok = false;
    crd::containers::String                           error_message;
    crd::containers::Array<crd::containers::String>   emitted_paths;

    explicit CookResult(crd::memory::IAllocator* a) noexcept;
};

[[nodiscard]] CookResult cook_helpers_prelude(
    crd::containers::StringView output_dir,
    crd::memory::IAllocator*    alloc) noexcept;

[[nodiscard]] CookResult cook_ir(
    const FormulaIr&            ir,
    crd::containers::StringView name,
    crd::containers::StringView output_dir,
    crd::memory::IAllocator*    alloc) noexcept;
```

### Output layout (per the v9e-d plan)

```
<output_dir>/
  sdf_helpers.glsl       ← shared GLSL prelude
  sdf_helpers.hlsl       ← shared HLSL prelude
  <name>.glsl            ← `float <name>(vec3 p)`   — IR-emitted SDF function
  <name>.hlsl            ← `float <name>(float3 p)` — same in HLSL
```

The per-SDF file contains ONLY the SDF function (no prelude). Consumers
concatenate or `#include` the prelude alongside per-SDF files so the prelude
is loaded once per renderer, not per SDF. Mirrors Inigo Quilez's `iqlibs`
pattern and matches what Cerid's Phase 3.5+ DFAO pass will consume.

### Implementation (`src/cooker.cpp`)

Two entry points; both:

1. Validate inputs (non-empty name; `validate(ir).status == Ok` for `cook_ir`).
2. `fs::create_directories(output_dir)`.
3. Call into the existing emitters (`glsl_helpers_prelude()` /
   `hlsl_helpers_prelude()` / `emit_glsl_sdf_function(ir, name, alloc)` /
   `emit_hlsl_sdf_function(ir, name, alloc)`).
4. `fs::write_file_text(...)` for each output.
5. Record each written path into `result.emitted_paths` on success; record
   the first failure into `result.error_message` and short-circuit on
   failure.

Idempotent: calling either entry point twice into the same `output_dir`
overwrites the prior files with byte-identical content.

### Tests (`tests/geometry-shader-helpers/test_cooker.cpp`)

5 cases / 190 assertions:

1. **Prelude cook** — `cook_helpers_prelude` writes both files; on-disk
   content matches `glsl_helpers_prelude()` / `hlsl_helpers_prelude()`
   byte-for-byte.
2. **Manifest cook (21 goldens)** — for every golden manifest, `cook_ir`
   writes `<name>.glsl` + `<name>.hlsl`; on-disk content matches the
   emitter output exactly; signatures `float <name>(vec3 p)` and
   `float <name>(float3 p)` appear.
3. **Empty name** — `cook_ir(..., name="", ...)` returns
   `CookResult{ok=false, error_message≠empty}`; output dir is NOT created.
4. **Invalid IR** — empty IR (zero nodes) returns
   `CookResult{ok=false, error_message≠empty}` with no files written.
5. **Idempotent re-cook** — cooking the same IR twice into the same
   directory succeeds both times; second-cook file content is
   byte-identical to first-cook content.

Full module binary post-slice: **21 cases / 910 assertions PASS** (was 16
cases / 720 assertions → +5 cases / +190 assertions).

## Decisions pinned

- **Two entry points, not one** — `cook_helpers_prelude` and `cook_ir`
  are separate so a consumer can cook the prelude once and many SDFs many
  times without re-emitting the prelude per call. Matches the
  `#include sdf_helpers.glsl` consumption pattern.
- **Per-SDF file contains only the function (no prelude)** — keeps cooked
  output deduplicated. The prelude is the heavy text (~5 KB); per-SDF
  functions are 100-300 LOC.
- **`CookResult` reports first-failure path + message, not a list of
  errors** — the cooker short-circuits on the first I/O failure so callers
  see the exact path that broke. Multi-error reporting would add
  complexity for no benefit; if a write fails, the filesystem is the
  problem and all subsequent writes will fail too.
- **Idempotent via overwrite, not by skip-if-exists** — `write_file_text`
  always overwrites. This matches what a build-time cooker wants:
  re-running after a source change must produce the new content, not
  skip because the file exists.

## Deferred follow-ons (consumer-pull)

- **`v9e-d-toml`** — TOML manifest format + parser so designers can
  author SDFs as text files instead of C++ code. Ships when a
  designer-driven consumer arrives (likely Phase 3.5+ editor).
- **`v9e-d-cmake`** — CMake `crd_cook_sdf_manifest()` helper that
  registers a manifest as a build-time dependency + runs the cooker
  tool. Ships when the renderer DFAO pipeline lands.
- **`v9e-d-crdr-pack`** — pack cooked files into a CRDR asset bundle for
  runtime load. Ships when the resources loader needs it.

All three are settled-design follow-ons: the cooker library is the hard
part; manifest parsing, CMake integration, and asset packaging are
mechanical wrappers around the existing `cook_helpers_prelude` /
`cook_ir` entry points.

## What remains in v9e

- **v9e-close** — ADR-0076 §26 amendment locks shader-helpers decisions
  (formula-IR schema, GLSL/HLSL target version, ULP-conformance
  threshold 1 ULP, cooker integration model). System doc
  `docs/systems/geometry-shader-helpers.md`.

Then v9-close (Phase 3.1.7 v9 cluster wrap), v10 `-curves` (5 slices),
v11 transform-aware queries, and Phase 3.1.7 fully closes.

## Files added

- `engine/geometry-shader-helpers/include/crd/geometry/shader_helpers/cooker.hpp` (~80 LOC public API)
- `engine/geometry-shader-helpers/src/cooker.cpp` (~190 LOC implementation)
- `tests/geometry-shader-helpers/test_cooker.cpp` (~210 LOC, 5 cases / 190 assertions)

## Files changed

- `engine/geometry-shader-helpers/CMakeLists.txt` — added `crd-platform`
  as PRIVATE link dep (cooker uses `crd::platform::fs::write_file_text` +
  `create_directories`).
- `docs/phases/phase-3.1.7-geometry.md` — v9e-d row marked ✅
  2026-05-19.
