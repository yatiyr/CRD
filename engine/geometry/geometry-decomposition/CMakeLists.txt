# crd-geometry-decomposition — Phase 3.1.7 v9c.
#
# Volumetric-Hierarchical Approximate Convex Decomposition (V-HACD, Mamou 2014).
# Cooker-only — not runtime; consumer is eylem v1c convex-collider conditioning.
# Future home for OBB-tree decomposition and other volumetric pre-process passes.
#
#   v9c-a  voxelize_mesh                Triangle mesh → 3D voxel grid (Mamou §3.1).
#                                       Akenine-Möller 2001 SAT surface marking
#                                       + WindingNumber (default, Jacobson 2013)
#                                       OR FloodFill classification.
#   v9c-b  vhacd_decompose              Recursive plane-search convex decomposition
#                                       (Mamou §3.2-3.4); cost = α·∂(R) + β·∂(R̄);
#                                       per-cluster hull via v3c Quickhull.
#   v9c-close                           ADR-0076 §24 amendment + system doc + eylem
#                                       v1c stub integration smoke.
#
# Surface marking uses exact SAT (not Mamou's conservative centroid-classification)
# because the substrate must also serve CAD / SDF generation where conservative
# overlap matters. The downstream v9c-b decompose stage receives a strictly better
# voxelization than the original paper assumes; cost-function tuning may need a
# one-pass calibration at v9c-b.
#
# Two-layer typing per ADR-0078 §5 D34: raw `<MathScalar T>` algorithm bodies +
# typed `Length<T>` voxel_size at the public surface (deferred to v9c-b/close
# per advisor scope-trim).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-decomposition STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-decomposition
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-decomposition
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-geometry-primitives
        crd-geometry-mesh
        crd-geometry-convex
    PRIVATE
        # v9c-a parallel surface-marking dispatches per-triangle-batch via
        # crd::jobs::parallel_for. PRIVATE — consumers of voxelize.hpp do
        # NOT need to include jobs headers.
        crd-jobs
)

target_compile_features(crd-geometry-decomposition PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-decomposition)
