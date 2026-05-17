#pragma once

// crd-geometry-delaunay — umbrella header.
//
// Phase 3.1.7 v8: pure 2D + 3D Delaunay triangulation + Voronoi extraction.
// v8a `delaunay_2d` substrate. v8b `delaunay_2d_hilbert` Hilbert-sorted
// variant. v8c-v8h algorithms ship progressively in sub-slices.

#include <crd/geometry/delaunay/delaunay_2d.hpp>
#include <crd/geometry/delaunay/delaunay_2d_hilbert.hpp>
#include <crd/geometry/delaunay/delaunay_3d.hpp>
#include <crd/geometry/delaunay/voronoi_2d.hpp>
#include <crd/geometry/delaunay/voronoi_3d.hpp>
#include <crd/geometry/delaunay/lloyd_2d.hpp>
#include <crd/geometry/delaunay/lloyd_3d.hpp>
#include <crd/geometry/delaunay/nni_2d.hpp>
#include <crd/geometry/delaunay/ruppert_2d.hpp>
#include <crd/geometry/delaunay/tet_refine_3d.hpp>
