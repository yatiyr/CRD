crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-eylem STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-eylem
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# v1a (interface module) keeps deps minimal: only what the public types touch.
# v1b adds crd-jobs (parallel solve) + crd-scene (component registration).
target_link_libraries(crd-eylem
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
)

target_compile_features(crd-eylem PUBLIC cxx_std_20)
crd_enable_default_pch(crd-eylem)
