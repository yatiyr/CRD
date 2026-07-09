# crd-gpu-context-dx12 — Phase 3.1.6 v17-i (ADR-0100): the DirectX 12 implementation of crd::gpu::IComputeContext — a
# SECOND compute backend behind the same backend-agnostic dispatch surface geometry + CKIR consume. Raw D3D12 compute +
# dxc (HLSL → DXIL), no rendering. WINDOWS-ONLY (D3D12); self-skips elsewhere so Linux/macOS builds are unaffected.

if(NOT WIN32)
    message(STATUS "crd-gpu-context-dx12: not Windows -- skipping the D3D12 compute backend.")
    return()
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-gpu-context-dx12 STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-gpu-context-dx12
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-gpu-context-dx12
    PUBLIC
        crd-gpu-context
        crd-containers
        d3d12
        dxgi
        dxguid
    PRIVATE
        crd-core
        crd-memory
)

target_compile_features(crd-gpu-context-dx12 PUBLIC cxx_std_20)
crd_enable_default_pch(crd-gpu-context-dx12)
