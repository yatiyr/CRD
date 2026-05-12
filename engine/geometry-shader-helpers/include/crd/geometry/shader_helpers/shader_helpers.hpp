#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-shader-helpers — Phase 3.1.7 v0e (skeleton; full impl = v9e).
//
// Purpose (ADR-0076 §1, sub-module 11): a GLSL/HLSL library that mirrors the
// C++ primitive distance functions + the iq formulary (smin/domain ops, see
// `crd/geometry/primitives/formulary.hpp`) so the renderer's DFAO / DF-soft-
// shadow passes (Phase 3.5+), font MTSDF, and the editor preview can evaluate
// the *same* SDF math on the GPU. The library is *cooked* — a build-time tool
// reads a formula-IR manifest (seeded here in v0e) and emits the shader source,
// then a ULP-conformance test checks the emitted GLSL/HLSL against the C++
// scalar reference (the `formulary.hpp` functions are that reference).
//
// v0e ships only the module skeleton + this header. The formula-IR types, the
// cooker, and the GLSL/HLSL backends are v9e. Until then this TU exists so the
// static library is a real link target (ASan / the SIMD-emission CI checks want
// an .obj) and so consumers can already name the module in their link lines.
// ---------------------------------------------------------------------------

namespace crd::geometry::shader_helpers
{
// Force-link anchor — defined in shader_helpers.cpp. Keeps the (currently empty)
// static library a real link target until v9e fills it in.
int force_link_geometry_shader_helpers() noexcept;
} // namespace crd::geometry::shader_helpers
