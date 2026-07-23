# ⛔ FROZEN (ADR-0105, 2026-07-23): crd-renderer is RETIRING — the D-007 RET band absorbs its capabilities into
# crd-gpu-context and DELETES this module at RET-8. NO new features, NO new consumers, NO new wiring. Ever.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-renderer STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-renderer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-renderer
    PUBLIC
        crd-core
        crd-math
        crd-rhi
        crd-shader
        crd-resources
        crd-scene
        crd-preset
)

target_compile_features(crd-renderer PUBLIC cxx_std_20)
crd_enable_default_pch(crd-renderer)
