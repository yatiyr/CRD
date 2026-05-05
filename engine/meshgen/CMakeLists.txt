crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-meshgen STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-meshgen
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-meshgen
    PUBLIC
        crd-core
        crd-math
        crd-memory
        crd-renderer
)

target_compile_features(crd-meshgen PUBLIC cxx_std_20)
crd_enable_default_pch(crd-meshgen)
