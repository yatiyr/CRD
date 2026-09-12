# crd-kir-dx12 — Phase 3.1.6 v17-d: the CKIR DirectX 12 backend. `KirBackendDx12` over raw D3D12 compute + dxc (HLSL →
# DXIL). A SEPARATE module (ADR-0096 lean-consumer). WINDOWS-ONLY (D3D12) — skipped elsewhere so Linux/macOS build
# unaffected. No external install (Windows SDK + dxcompiler.dll). ADR-0098.

if(NOT WIN32)
    message(STATUS "crd-kir-dx12: not Windows -- skipping the D3D12 backend.")
    return()
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir-dx12 STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-kir-dx12
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-kir-dx12
    PUBLIC
        crd-kir
        crd-core
        crd-containers
        crd-memory
        d3d12
        dxgi
        dxguid   # D3D12/dxc CLSIDs + IIDs
    PRIVATE
        crd-warnings
)

target_compile_features(crd-kir-dx12 PUBLIC cxx_std_20)
crd_enable_default_pch(crd-kir-dx12)
