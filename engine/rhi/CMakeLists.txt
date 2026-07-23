# ⛔ FROZEN (ADR-0105, 2026-07-23): crd-rhi is RETIRING — the D-007 RET band absorbs its capabilities into
# crd-gpu-context and DELETES this module at RET-8. NO new features, NO new consumers, NO new wiring. Ever.
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
        crd-gpu-context   # D-008 C2-d2: Device::create_program returns crd::gpu::IGpuProgram (header-only; acyclic)
)

target_compile_features(crd-rhi PUBLIC cxx_std_20)
crd_enable_default_pch(crd-rhi)
