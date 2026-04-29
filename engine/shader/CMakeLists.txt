crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-shader STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-shader
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-shader
    PUBLIC
        crd-core
        crd-containers
        crd-rhi
)

target_compile_features(crd-shader PUBLIC cxx_std_20)
crd_enable_default_pch(crd-shader)
