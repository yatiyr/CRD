# crd-geometry-mesh-processing — Phase 3.1.7 v7.
#
# Mesh-processing substrate: the 8th `crd-geometry-*` sub-module. Built on
# `crd-geometry-mesh`'s read-only TriangleMeshView; this module is the
# MUTABLE side — algorithms that "improve" or "fix" a triangle mesh.
#
#   v7a  HalfEdgeMesh<T>           the mutable mesh data structure (this slice).
#   v7b  qem_decimate              Garland-Heckbert 1997 quadric edge collapse decimation.
#   v7c  loop_subdivide            Loop 1987 subdivision.
#   v7d  isotropic_remesh          Botsch-Kobbelt 2004.
#   v7e  fill_holes                Liepa 2003 weighted minimum-area triangulation.
#   v7f  repair_manifoldness       non-manifold edge split + bowtie vertex duplication.
#   v7g  remove_self_intersections triangle-triangle intersection cut-and-retri
#                                   (consumes v6e Bentley-Ottmann).
#   v7h  taubin_smooth             volume-preserving Laplacian smoothing (Taubin 1995).
#
# Two-layer typing per ADR-0078 §5 D34: algorithm bodies stay raw
# `<MathScalar T>`; typed `Vec3<Length32>` consumers ride strip-compute-
# retag wrappers in `*_typed.hpp` headers (added at slice close if a
# typed-surface consumer pulls).
#
# Robustness contract: every orientation / intersection / link-condition /
# in-segment decision uses Shewchuk adaptive predicates (v3a, ADR-0076 §18).
# Builder reject (non-finite + degenerate at construction) + query tolerate.
# Determinism (ADR-0063 + ADR-0076 §4 pin #11): lex-tuple ordering for edge
# enumeration + collapse-cost min-heap + face / vertex iteration.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-mesh-processing STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-mesh-processing
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-mesh-processing
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
        crd-geometry-bvh
        crd-geometry-mesh
        crd-geometry-polygon
)

target_compile_features(crd-geometry-mesh-processing PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-mesh-processing)
