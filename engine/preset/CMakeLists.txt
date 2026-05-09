crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-preset STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-preset
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-preset
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-resources
)

target_compile_features(crd-preset PUBLIC cxx_std_20)
crd_enable_default_pch(crd-preset)
