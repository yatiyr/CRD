crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-lod STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-lod
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# REN-40-C1: the seam this module exists to hold. `crd-resources` is deliberately
# math-module-free, and `crd-geometry-mesh-processing` deliberately knows nothing
# about resources — so the code that turns a MeshResource into an LOD chain cannot
# live in either of them without inverting a dependency. It lives here.
target_link_libraries(crd-lod
    PUBLIC
        crd-core
        crd-math
        crd-memory
        crd-resources
    PRIVATE
        crd-geometry-mesh-processing
)

target_compile_features(crd-lod PUBLIC cxx_std_20)
crd_enable_default_pch(crd-lod)
