# crd-geometry-polygon — Phase 3.1.7 v6.
#
# Polygon-ops substrate: the 8th `crd-geometry-*` sub-module. Five algorithmic
# slices, all robust under Shewchuk adaptive predicates from day 1:
#
#   v6a  Polygon2<T> / PolygonView2<T> / Ring2<T> substrate + signed_area /
#        centroid / aabb / is_ccw / is_simple / point_in_polygon. THIS SLICE.
#   v6b  Held 2001 FIST ear-clipping triangulation with hole support (Eberly
#        cut-and-join bridges). Output: deterministic triangle list.
#   v6c  Constrained Delaunay Triangulation (Bowyer-Watson + Anglada 1997
#        segment recovery). Shewchuk incircle drives the in-circle tests.
#   v6d  Vatti polygon Boolean (Clipper2-grade union/intersection/difference/
#        XOR for multipath/multipolygon w/ holes). Scanbeam-table sweep +
#        scanbeam Y-sort.
#   v6e  Bentley-Ottmann line-segment intersection sweep (degenerate-aware:
#        vertical segments + 3+ at point + collinear-overlap).
#
# Two-layer typing per ADR-0078 §5: algorithm bodies stay raw `<MathScalar T>`
# on `f32`/`f64`; typed `Vec2<Length32>` / `Length32` consumers ride
# `*_typed.hpp` strip-compute-retag wrappers. Determinism:
# `crd::containers::sort` / `push_heap` / `pop_heap` with lex-tuple
# comparators; Shewchuk-only adverse path for every orientation /
# intersection / in-circle decision (no naive cross-product fallback);
# vertex-on-edge ⇒ Clipper2 convention.
#
# Builder reject (non-finite ⇒ debug CRD_ASSERT at entry); query tolerate
# (non-finite ⇒ defensive `is_finite` short-circuit, no crash).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-polygon STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-polygon
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-polygon
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
)

target_compile_features(crd-geometry-polygon PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-polygon)
