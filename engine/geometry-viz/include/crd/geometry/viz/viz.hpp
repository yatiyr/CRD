#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-viz — debug-draw companion for crd-geometry (Phase 3.1.7 v1j-a).
//
// Pure data-emitting adapters that take `crd::geometry::primitives::*` types
// and `crd::geometry::bvh::*` trees and emit `crd::draw::RenderBuffer` records.
// crd-geometry itself never links crd-draw — a headless / cooker / DAW build
// can consume the geometry substrate without pulling the GPU debug-draw layer.
// This module is the bridge that knows about BOTH; consumers (the sandbox,
// future editor, etc.) link it explicitly when they want geometry visualised.
//
// Layered surface:
//   primitives.hpp - overloaded `draw(buf, Shape, ...)` for every concrete
//                    primitive. Forwards to existing `crd::draw::*_to`
//                    helpers (`aabb_wire_to`, `sphere_wire_to`, etc.).
//   queries.hpp    - draw_ray_hit (ray + hit point + optional normal),
//                    draw_closest_point (query→closest segment + endpoints),
//                    draw_normals (per-vertex / per-face hairs).
//   bvh.hpp        - draw_bvh(BvhTree | Bvh4Tree | DynamicBvh) (depth-keyed
//                    AABB walk), draw_overlap_pairs(DynamicBvh) (lines
//                    between overlapping leaf centroids), draw_frustum_cull
//                    (BvhTree / kept-vs-culled two-colour).
// ---------------------------------------------------------------------------

#include <crd/geometry/viz/bvh.hpp>
#include <crd/geometry/viz/primitives.hpp>
#include <crd/geometry/viz/queries.hpp>
