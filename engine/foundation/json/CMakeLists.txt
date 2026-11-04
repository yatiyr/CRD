# crd-json -- the engine's own JSON (RFC 8259): the flat-DOM reader (json.hpp) and the streaming writer
# (json_write.hpp), allocator-aware and span-based. Moved out of crd-asset-io on 2026-11-04 (capability text.json), so
# ceridc, the diagnostic tests and the crypto vectors read JSON without linking the geometry importers behind it.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-json STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-json
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-json
    PUBLIC
        crd-core
        crd-containers
        crd-memory
)

target_compile_features(crd-json PUBLIC cxx_std_20)
crd_enable_default_pch(crd-json)
