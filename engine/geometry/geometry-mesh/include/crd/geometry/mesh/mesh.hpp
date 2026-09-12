#pragma once

// crd-geometry-mesh — umbrella include.
// Phase 3.1.7 v4: triangle-mesh queries (closest-point, raycast, winding).
// v4a ships TriangleMeshView + per-mesh BVH builder + closest_point query.

#include <crd/geometry/mesh/mesh_bvh.hpp>
#include <crd/geometry/mesh/mesh_closest_point.hpp>
#include <crd/geometry/mesh/mesh_raycast.hpp>
#include <crd/geometry/mesh/mesh_raycast_simd.hpp>
#include <crd/geometry/mesh/mesh_validate.hpp>
#include <crd/geometry/mesh/mesh_winding_number.hpp>
#include <crd/geometry/mesh/triangle_mesh.hpp>
