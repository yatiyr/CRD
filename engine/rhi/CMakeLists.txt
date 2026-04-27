crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-rhi STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-rhi
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-rhi
    PUBLIC
        crd-core
        crd-memory
        crd-platform
)

target_compile_features(crd-rhi PUBLIC cxx_std_20)
crd_enable_default_pch(crd-rhi)
