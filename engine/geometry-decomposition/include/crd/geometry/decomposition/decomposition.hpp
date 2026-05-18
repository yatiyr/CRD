#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-decomposition umbrella — Phase 3.1.7 v9c.
//
// Volumetric-Hierarchical Approximate Convex Decomposition (V-HACD, Mamou
// 2014) + future volumetric decomposition primitives. Cooker-only — not
// runtime; primary consumer is eylem v1c convex-collider conditioning.
//
// v9c-a  voxel.hpp + voxelize.hpp     triangle mesh → 3D voxel grid
// v9c-b  vhacd_decompose.hpp           recursive plane-search convex decomp
//
// Two-layer typing per ADR-0078 §5 D34: raw `<MathScalar T>` algorithm
// bodies + typed `Length<T>` at public surface (typed wrappers shipped at
// v9c-b/close per advisor scope-trim).
// ---------------------------------------------------------------------------

#include <crd/geometry/decomposition/voxel.hpp>
#include <crd/geometry/decomposition/voxelize.hpp>
#include <crd/geometry/decomposition/vhacd.hpp>
