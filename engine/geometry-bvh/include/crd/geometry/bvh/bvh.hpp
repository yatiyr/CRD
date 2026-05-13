#pragma once

// ---------------------------------------------------------------------------
// crd-geometry-bvh — bounding-volume-hierarchy sub-module of crd-geometry
// (ADR-0076 §1, second sub-module after `-primitives`).
//
// v1a: `BvhTree` container + binned-SAH binary builder (`bvh_build`) + ordered
// nearest-hit raycast (`bvh_raycast`) + AABB-overlap query (`bvh_overlap`),
// over `AABB3<f32>` primitive boxes. Functional API form (ADR-0076 §11):
// `bvh_build(span, alloc) → BvhTree`, not a `BvhTree::build` member.
// v1b: `bvh_refit` — O(n) bottom-up AABB recomputation, topology untouched.
// v1c: `DynamicBvh` — the incrementally-updatable AABB tree (insert/remove/update
//      via Catto-2019 height-balanced tree rotations; fat-AABB margin). This is
//      a different structure from the static `BvhTree` — `dynamic_bvh.hpp`.
// v1d: `Bvh4Tree` + `bvh4_collapse(BvhTree)` — the 4-wide topology variant
//      (collapse a built binary tree into ≤4-child nodes) + scalar ray-vs-4-AABB
//      `bvh4_raycast` / `bvh4_overlap` — `bvh4.hpp`.
// v1e: `bvh_closest_point` — branch-and-bound closest-point over the binary tree.
// v1f: `bvh_build_parallel` — jobs-parallel binned-SAH build, bit-identical to
//      the serial `bvh_build` — `bvh_build_parallel.hpp`.
// v1g: `Vec4f` ray-vs-4-AABB kernel (`bvh4_simd.hpp`) — `bvh4_raycast`'s per-node
//      test; BVH4 is now the recommended traversal form for static query-heavy
//      data. (`BvhBuildOptions::topology` removed — `bvh_build` is binary,
//      `bvh4_collapse` is the BVH4 path.)
//
// Roadmap: v1h primitives-substrate hardening · v1i query facade + shapecast ·
// v1j `crd-geometry-viz`.
//
// Determinism: inherits ADR-0076 §4 / §5.2 — the SAH split tiebreak is pinned
// (X → Y → Z axis, lower bin index first), the build uses `crd::containers`
// (no `std::sort`), traversal is a fixed-order stack walk → identical trees and
// query results bit-for-bit across configs.
// ---------------------------------------------------------------------------

#include <crd/geometry/bvh/bvh4.hpp>
#include <crd/geometry/bvh/bvh4_simd.hpp>
#include <crd/geometry/bvh/bvh_build.hpp>
#include <crd/geometry/bvh/bvh_build_parallel.hpp>
#include <crd/geometry/bvh/bvh_query.hpp>
#include <crd/geometry/bvh/bvh_shapecast.hpp>
#include <crd/geometry/bvh/bvh_tree.hpp>
#include <crd/geometry/bvh/bvh_update.hpp>
#include <crd/geometry/bvh/dynamic_bvh.hpp>
