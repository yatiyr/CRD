crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-resources STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-resources
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-resources
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-log
        crd-platform
    PRIVATE
        libzstd_static
)

target_include_directories(crd-resources
    PRIVATE
        "${zstd_SOURCE_DIR}/lib"
)

target_compile_features(crd-resources PUBLIC cxx_std_20)

crd_enable_default_pch(crd-resources)
