#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — Cooker. Phase 3.1.7 v9e-d (2026-05-19).
//
// Emits a formula-IR (or the shared helpers prelude) to .glsl + .hlsl files
// on disk. The runtime / cooker / build-time tool consuming this writes a
// library of cooked SDF helpers that any renderer pipeline can `#include`
// directly — no per-consumer code-generation step.
//
// **Output structure** (per the v9e-d plan):
//
//   <output_dir>/
//     sdf_helpers.glsl       ← the shared GLSL prelude (every `sd_*` + smin/op/domain)
//     sdf_helpers.hlsl       ← same in HLSL 6.0
//     <name>.glsl            ← `float <name>(vec3 p)`   — IR-emitted SDF function
//     <name>.hlsl            ← `float <name>(float3 p)` — same in HLSL
//
// The per-SDF file contains ONLY the SDF function (no prelude). Consumers
// concatenate or #include the prelude alongside per-SDF files so the prelude
// is loaded once per renderer, not per SDF. This is the canonical "library
// of cooked SDFs" pattern Inigo Quilez's `iqlibs`/iquilezles.org references
// use, and the pattern Cerid's Phase 3.5+ DFAO pass will consume.
//
// **Deferred (consumer-pull):**
//   - **v9e-d-toml** — TOML manifest format + parser so designers can author
//     SDFs as text files instead of C++ code. Ships when a designer-driven
//     consumer arrives.
//   - **v9e-d-cmake** — CMake `crd_cook_sdf_manifest()` helper that registers
//     a manifest as a build-time dependency + runs the cooker tool. Ships
//     when the renderer DFAO pipeline lands.
//   - **v9e-d-crdr-pack** — pack cooked files into a CRDR asset bundle for
//     runtime load. Ships when the resources loader needs it.
// ---------------------------------------------------------------------------

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::shader_helpers
{

struct CookResult
{
    bool                                          ok = false;
    crd::containers::String                       error_message;
    crd::containers::Array<crd::containers::String> emitted_paths;

    explicit CookResult(crd::memory::IAllocator* a) noexcept
        : error_message(a), emitted_paths(a)
    {
    }
};

// Write the shared helpers prelude:
//   <output_dir>/sdf_helpers.glsl
//   <output_dir>/sdf_helpers.hlsl
// `output_dir` is created if it doesn't exist. Idempotent — repeated calls
// overwrite the same files with the same content.
[[nodiscard]] CookResult cook_helpers_prelude(
    crd::containers::StringView output_dir,
    crd::memory::IAllocator*    alloc) noexcept;

// Write one named SDF function for one IR:
//   <output_dir>/<name>.glsl  — `float <name>(vec3 p)` — references helpers
//                                  from sdf_helpers.glsl
//   <output_dir>/<name>.hlsl  — `float <name>(float3 p)` — same in HLSL
//
// Pre-condition: `validate(ir).status == Ok`. The IR walker asserts; the
// cooker returns CookResult{ok=false, error_message=...} on filesystem errors.
[[nodiscard]] CookResult cook_ir(
    const FormulaIr&            ir,
    crd::containers::StringView name,
    crd::containers::StringView output_dir,
    crd::memory::IAllocator*    alloc) noexcept;

} // namespace crd::geometry::shader_helpers
