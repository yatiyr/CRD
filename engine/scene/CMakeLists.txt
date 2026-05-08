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
        crd-resources
)

target_compile_features(crd-scene PUBLIC cxx_std_20)
crd_enable_default_pch(crd-scene)
