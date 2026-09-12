# crd-geometry-spatial — Phase 3.1.7 v5.
#
# Spatial-acceleration substrate beyond the per-primitive BVH already in
# crd-geometry-bvh. Five backends, one query facade:
#
#   v5a  KdTree<T>          static balanced KD-tree over a point set
#                            (Friedman/Bentley/Finkel 1977 + nanoflann/PCL
#                             widest-extent split heuristic + leaf-bucket).
#   v5b  LooseOctree<T>      Ulrich 2000 dynamic AABB index — the workhorse
#                            for scene spatial culling.
#   v5c  RTree<T>            Beckmann 1990 R*-tree — better packing than
#                            octree for cooked/static level data.
#   v5d  SpatialHash<T>      Teschner 2003 fixed-grid hash — particle/swarm
#                            broadphase.
#   v5e  UniformGrid<T>      dense 3D cell array for small bounded domains.
#
# This slice (v5a) ships KdTree only; the four other backends arrive in
# v5b–v5e. v5-queries-extension widens crd/geometry/queries.hpp's compile-
# time-overload facade across all backends.
#
# Two-layer typing per ADR-0078 §5: algorithm bodies stay raw `<MathScalar T>`
# in `kd_*.hpp` / `kd_*.cpp`; typed `Vec3<Length32>` / `Length32` consumers
# ride `kd_queries_typed.hpp` strip-compute-retag wrappers. Determinism:
# `crd::containers::nth_element` (deterministic median-of-three quickselect,
# bit-exact across MSVC / GCC / clang) drives the build; lex-tuple
# (coord, original-index) comparator means no two elements compare equal,
# so the resulting tree topology + leaf order is byte-identical across
# stdlib implementations. Lowest-payload-index tiebreak on equal squared
# distance per ADR-0076 §4 pin #11.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-spatial STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-spatial
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-spatial
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
)

target_compile_features(crd-geometry-spatial PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-spatial)
