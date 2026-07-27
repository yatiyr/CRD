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
        crd-anim           # GEO-8: clip sampling + skin palettes for the skinned draw path
        crd-gpu-context
        crd-kir
        crd-frame-cook     # REN-37.10: the renderer HOSTS an authored frame graph rather than building passes
        crd-vertex-cook    # REN-38-D5: every vertex program is COOKED FROM A `.crdv`, not compiled in
        crd-material-cook  # REN-38-C4: the scene SURFACE is cooked from an authored `.crdm`
        crd-light-cook     # REN-38-E7: the LIGHTING technique body is an authored declaration
        crd-log            # REN-37.10: a graph that fails to record must REPORT which pass and why
)

target_compile_features(crd-scene-render PUBLIC cxx_std_20)

crd_enable_default_pch(crd-scene-render)
