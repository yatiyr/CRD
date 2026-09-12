# crd-geometry-primitives — Phase 3.1.7 v0a.
#
# First sub-module of crd-geometry (ADR-0076 §1). Primitive shape types +
# closest-point / intersection / containment helpers (v0a parks the catalogue
# migrated from the deleted crd/math/geometry.hpp; v0b–v0f extend it). Header-
# only templates today; geometry_primitives.cpp is a force-link anchor that
# becomes a real TU once v0c+ adds out-of-line SIMD batch kernels.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-primitives STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-primitives
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-primitives
    PUBLIC
        crd-core        # types, asserts
        crd-math        # Vec/Mat/Quat + scalar helpers (dot/cross/length/clamp/...) + format base
        crd-containers  # StaticArray (Frustum's 6 planes); the canonical owning/view containers per ADR-0076 §5
        crd-simd-flags
)

if(MSVC)
    # Guarded reciprocal code in intersect_ray_triangle stays non-zero at
    # runtime, but MSVC can't prove it under LTCG and emits a conservative
    # C4723. Scoped here (moved off crd-math, which no longer ships geometry).
    target_compile_options(crd-geometry-primitives PUBLIC /wd4723)
endif()

target_compile_features(crd-geometry-primitives PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-primitives)
