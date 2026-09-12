# crd-render-material — RAF-5 (D-007 "RAF band"; mission §5 · ADR-0102 / REN-37).
#
# The MATERIAL + TECHNIQUE runtime contract, enforcing the separation as a type system: material = surface response
# (a definition + lightweight instances; structurally CANNOT touch lighting), technique = shading algorithm (consumes
# surface channels, supports render phases, reaches lighting itself), and the deterministic program-variant key +
# cache (many instances share one variant). Backend-/IR-agnostic; validated with no device.
# Leaf-ish module — deps: core / containers / memory / render-asset-core / render-program.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-render-material STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-render-material
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-render-material
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-render-asset-core   # AssetId · Diagnostic
        crd-render-program      # VariantKey
)

target_compile_features(crd-render-material PUBLIC cxx_std_20)
crd_enable_default_pch(crd-render-material)
