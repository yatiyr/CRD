# crd-geometry-mesh — Phase 3.1.7 v4.
#
# Triangle-mesh queries: closest-point (v4a) + raycast (v4b) + Jacobson 2013
# winding number (v4c) + per-leaf SIMD Möller-Trumbore (v4d) + mesh validation
# (v4-validate). Consumes the v1 -bvh substrate (per-triangle AABB tree) plus
# v0 -primitives (Ericson cascade closest-point on Triangle3).
#
# Per ADR-0078 §5 D34, the raw `<MathScalar T>` algorithm bodies stay raw;
# typed `<Quantity<D, T>>` consumers go through `mesh_queries_typed.hpp`.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-mesh STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-mesh
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-mesh
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
        crd-geometry-bvh
)

target_compile_features(crd-geometry-mesh PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-mesh)
