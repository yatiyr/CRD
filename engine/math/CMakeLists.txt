crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-math STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-math
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-math
    PUBLIC
        crd-core
        crd-simd-flags
)

target_compile_features(crd-math PUBLIC cxx_std_20)
crd_enable_default_pch(crd-math)
