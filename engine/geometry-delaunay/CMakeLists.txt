# crd-geometry-delaunay — Phase 3.1.7 v8.
#
# Pure 2D + 3D Delaunay triangulation + Voronoi diagram extraction. 9th
# `crd-geometry-*` sub-module. Standalone primitive — `crd-geometry-polygon`'s
# v6c `constrained_delaunay` is a higher-level operation that adds constraint
# handling on top of the same Bowyer-Watson core; an optional follow-on slice
# at v8-close can refactor v6c to consume v8a internally.
#
#   v8a  delaunay_2d                 2D Bowyer-Watson lex-sort incremental.
#   v8b  delaunay_2d_hilbert         2D Bowyer-Watson Hilbert-sort (delaunator-style).
#   v8c  delaunay_3d                 3D Bowyer-Watson tetrahedralisation (consumes v8c-pre insphere Stage D).
#   v8d-2d voronoi_2d                2D Voronoi diagram (geometric dual of v8a/v8b).
#   v8d-3d voronoi_3d                3D Voronoi diagram (geometric dual of v8c) + ConvexHullView helper.
#   v8c-pre insphere_exact Stage D   Mandatory paydown before v8c.
#   v8c  delaunay_3d                 3D Bowyer-Watson tetrahedralisation.
#   v8d-2d voronoi_2d                2D Voronoi diagram extraction.
#   v8d-3d voronoi_3d                3D Voronoi cells extraction.
#   v8e  lloyd_cvt                   Lloyd 1982 Centroidal Voronoi iteration.
#   v8f  natural_neighbour           Sibson 1981 Natural Neighbour Interpolation.
#   v8g  ruppert_refine              Ruppert 1995 2D Delaunay quality refinement.
#   v8h  delaunay_3d_refine          3D Delaunay refinement (sliver removal).
#
# Two-layer typing per ADR-0078 §5 D34: raw `<MathScalar T>` algorithm bodies +
# typed `Vec2<Length32>` / `Vec3<Length32>` consumers ride strip-compute-retag
# wrappers in `*_typed.hpp` headers added at slice close on first typed
# consumer.
#
# Robustness contract: every orientation / circumcircle / circumsphere
# decision uses Shewchuk adaptive predicates (v3a, ADR-0076 §18). Builder
# reject (non-finite + duplicate point at construction) + query tolerate.
# Determinism (ADR-0063 + ADR-0076 §4 pin #11): lex-tuple `(x, y,
# original_index)` insertion order; lex tiebreak on every comparator;
# bit-identical output across compilers given byte-identical input.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-delaunay STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-delaunay
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-delaunay
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
)

target_compile_features(crd-geometry-delaunay PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-delaunay)
