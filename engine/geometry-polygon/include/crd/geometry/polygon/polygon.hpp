#pragma once

// crd-geometry-polygon — umbrella include.
//
// Phase 3.1.7 v6: planar-polygon substrate.
//   v6a ships Polygon2 / PolygonView2 / Ring2 + signed_area / centroid / aabb /
//       is_ccw / is_simple / point_in_polygon + typed Quantity wrappers.
//   v6b–v6e add ear clipping (Held FIST) / CDT / Vatti Boolean / Bentley-Ottmann.

#include <crd/geometry/polygon/bentley_ottmann.hpp>
#include <crd/geometry/polygon/cdt.hpp>
#include <crd/geometry/polygon/polygon_boolean.hpp>
#include <crd/geometry/polygon/polygon_predicates.hpp>
#include <crd/geometry/polygon/polygon_predicates_typed.hpp>
#include <crd/geometry/polygon/polygon_types.hpp>
#include <crd/geometry/polygon/triangulate_ear_clip.hpp>
