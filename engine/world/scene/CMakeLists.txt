crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-scene STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-scene
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-scene
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-geometry-primitives  # query.hpp's frozen spatial-DSL operators take crd::geometry::primitives::AABB<f32> (ADR-0076 §13)
        crd-geometry-spatial     # SpatialBVHIndex backs onto LooseOctree (Phase 3.1.7 v5-index-bringup, ADR-0076 §20)
        crd-resources
)

target_compile_features(crd-scene PUBLIC cxx_std_20)
crd_enable_default_pch(crd-scene)
