#pragma once

// crd-geometry-spatial — umbrella include.
//
// Phase 3.1.7 v5: spatial-acceleration substrate beyond per-primitive BVH.
// v5a ships KdTree (point set + nearest-N + radius + AABB-window range).
// v5b–v5e add LooseOctree / RTree / SpatialHash / UniformGrid.

#include <crd/geometry/spatial/kd_nearest_n.hpp>
#include <crd/geometry/spatial/kd_queries_typed.hpp>
#include <crd/geometry/spatial/kd_radius.hpp>
#include <crd/geometry/spatial/kd_range_aabb.hpp>
#include <crd/geometry/spatial/kd_tree.hpp>
#include <crd/geometry/spatial/loose_octree.hpp>
#include <crd/geometry/spatial/octree_queries_typed.hpp>
#include <crd/geometry/spatial/rtree.hpp>
#include <crd/geometry/spatial/rtree_queries_typed.hpp>
#include <crd/geometry/spatial/spatial_hash.hpp>
#include <crd/geometry/spatial/hash_queries_typed.hpp>
#include <crd/geometry/spatial/uniform_grid.hpp>
#include <crd/geometry/spatial/grid_queries_typed.hpp>
