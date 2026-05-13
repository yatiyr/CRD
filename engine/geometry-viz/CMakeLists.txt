# crd-geometry-viz — Phase 3.1.7 v1j-a.
#
# Companion module (per ADR-0066 §13 dependency-inverted plug-in pattern):
# the bridge that knows about BOTH crd-geometry-* and crd-draw lives here.
# `crd-geometry-primitives` / `crd-geometry-bvh` themselves never link
# crd-draw, so a headless / cooker / DAW / scientific-computing build can
# consume the geometry substrate without pulling the GPU debug-draw layer.
#
# Surface (header-mostly + light .cpp for BVH-traversal walkers):
#   primitives.hpp  - overloaded `draw(RenderBuffer&, Shape, ...)` for every
#                     concrete primitive (AABB3 / OBB3 / Sphere / Capsule3 /
#                     Cylinder3 / Triangle3 / Tetrahedron / Plane patch /
#                     Ray3 / Segment3 / Line3). Forwards to crd::draw::*_to.
#   queries.hpp     - draw_ray_hit / draw_closest_point / draw_normals --
#                     query-result visualisations.
#   bvh.hpp         - draw_bvh(BvhTree | Bvh4Tree | DynamicBvh) walks the
#                     tree + emits depth-coloured node AABBs;
#                     draw_overlap_pairs(DynamicBvh) emits lines between
#                     overlapping leaf centroids; draw_frustum_cull(Frustum,
#                     BvhTree) two-colour visualisation.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-viz STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-viz
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-viz
    PUBLIC
        crd-core
        crd-math
        crd-containers
        crd-geometry-primitives
        crd-geometry-bvh
        crd-draw
)

target_compile_features(crd-geometry-viz PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-viz)
