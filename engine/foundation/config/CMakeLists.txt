crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-config STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-config
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-config
    PUBLIC
        crd-core
        crd-containers
        crd-log
        crd-math
        crd-platform
        crd-toml
)

target_compile_features(crd-config PUBLIC cxx_std_20)
crd_enable_default_pch(crd-config)
