#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — HLSL backend. Phase 3.1.7 v9e-c (2026-05-19).
//
// Mirror of `glsl_emitter.hpp` for HLSL 6.0. Same IR walker, different syntax.
// The cooker (v9e-d) emits both `.glsl` and `.hlsl` files from the same IR so
// a renderer can pick its target language per-backend without re-authoring.
//
// **Why HLSL too?** D3D12 / DirectX renderers need HLSL. Vulkan-only engines
// today (Cerid included) consume GLSL. A multi-backend renderer wants both.
// Once Cerid adds a D3D12 backend (Phase 3.5+), v9e-c output drops in
// directly — no shader re-authoring step.
//
// **By-construction correctness**: HLSL math here is line-for-line identical
// to the GLSL helpers in `glsl_emitter.cpp` — only the syntax (vec3 → float3,
// floatBitsToUint → asuint, etc.) differs. Since v9e-b proved C++ ≈ GLSL
// within ~1e-6 absolute on all 21 golden manifests via GPU dispatch, and
// HLSL ≡ GLSL by structural identity, HLSL ≈ C++ by induction. Full GPU
// verification of HLSL (via dxc → SPIR-V dispatch on Vulkan, or a future
// D3D12 backend) ships as `v9e-c-dxc-spirv-dispatch` when a consumer
// arrives — see the v9e-c-followons list.
// ---------------------------------------------------------------------------

#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/geometry/shader_helpers/formula_ir.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::geometry::shader_helpers
{

// HLSL prelude — every `sd_*` + smin/op/domain helper translated to HLSL 6.0.
[[nodiscard]] crd::containers::StringView hlsl_helpers_prelude() noexcept;

// Emit `float <function_name>(float3 p) { ... }` for the given IR.
// Pre-condition: `validate(ir).status == Ok`.
[[nodiscard]] crd::containers::String emit_hlsl_sdf_function(
    const FormulaIr&            ir,
    crd::containers::StringView function_name,
    crd::memory::IAllocator*    alloc) noexcept;

// Complete HLSL compute shader: prelude + sdf() + main() that samples a 3D
// grid into a structured-buffer output. Mirror of the GLSL conformance
// shader; the cooker outputs THIS so the renderer can compile + dispatch
// it directly (when a D3D12 / dxc-Vulkan path exists).
[[nodiscard]] crd::containers::String emit_hlsl_conformance_shader(
    const FormulaIr&         ir,
    crd::memory::IAllocator* alloc) noexcept;

} // namespace crd::geometry::shader_helpers
