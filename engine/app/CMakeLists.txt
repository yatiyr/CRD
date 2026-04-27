crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-app STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-app
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-app
    PUBLIC
        crd-core
        crd-containers
        crd-platform
)

target_compile_features(crd-app PUBLIC cxx_std_20)
crd_enable_default_pch(crd-app)
