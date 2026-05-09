crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-renderer STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-renderer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-renderer
    PUBLIC
        crd-core
        crd-math
        crd-rhi
        crd-shader
        crd-resources
        crd-scene
        crd-preset
)

target_compile_features(crd-renderer PUBLIC cxx_std_20)
crd_enable_default_pch(crd-renderer)
