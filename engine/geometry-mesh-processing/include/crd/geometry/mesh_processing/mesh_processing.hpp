#pragma once

// crd-geometry-mesh-processing — umbrella header.
//
// Phase 3.1.7 v7: mesh-processing substrate built on the half-edge data
// structure (v7a). v7b–v7h add the algorithms (QEM / Loop / remesh /
// hole-fill / manifoldness-repair / self-intersect-removal / Taubin).

#include <crd/geometry/mesh_processing/fill_holes.hpp>
#include <crd/geometry/mesh_processing/half_edge_mesh.hpp>
#include <crd/geometry/mesh_processing/isotropic_remesh.hpp>
#include <crd/geometry/mesh_processing/loop_subdivide.hpp>
#include <crd/geometry/mesh_processing/qem_decimate.hpp>
#include <crd/geometry/mesh_processing/quadric.hpp>
#include <crd/geometry/mesh_processing/remove_self_intersections.hpp>
#include <crd/geometry/mesh_processing/repair_manifoldness.hpp>
#include <crd/geometry/mesh_processing/taubin_smooth.hpp>
