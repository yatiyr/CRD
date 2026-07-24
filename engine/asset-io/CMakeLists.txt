# crd-asset-io — GEO-1/2/3 (D-007 rows 66-68): the import substrate.
# GEO-3 adds: json.hpp/.cpp (OUR JSON parser) + gltf.hpp/.cpp (OUR glTF 2.0 geometry/material parser, replacing cgltf).
#
# OUR OWN external-format parsers (STL now; OBJ/PLY/glTF/3MF follow the GEO band) filling the `ImportedAsset`
# intermediate model — the seam between "file bytes" and the decompose → condition → cook pipeline. Parsers are
# allocator-aware and span-based (no filesystem; the caller does I/O), zero 3rd-party per the engine doctrine.
# Research: docs/research/2026-07-23-geometry-resource-pipeline.md + -resource-scene-world-class.md.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-asset-io STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-asset-io
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-asset-io
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-time
        crd-geometry-primitives
        crd-geometry-bvh
        crd-geometry-mesh
)
