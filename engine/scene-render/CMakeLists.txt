crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-scene-render STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-scene-render
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-scene-render
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-geometry-primitives
        crd-scene
        crd-resources
        crd-gpu-context
        crd-kir
)

target_compile_features(crd-scene-render PUBLIC cxx_std_20)

crd_enable_default_pch(crd-scene-render)
