# crd-gpu-context-dx12 tests — Phase 3.1.6 v17-i (ADR-0100). Guarded: only built if the D3D12 compute backend target
# exists (Windows), so Linux/macOS builds are unaffected.
if(NOT TARGET crd-gpu-context-dx12)
    return()
endif()

add_executable(crd-gpu-context-dx12-tests
    test_dx12_compute.cpp
    test_dx12_gsplat.cpp
    test_dx12_raster.cpp
    test_dx12_rt.cpp
    ../gpu-shared/win32_test_window.cpp # RET-2: the isolated <windows.h> TU — a REAL window for the present gates
)

target_link_libraries(crd-gpu-context-dx12-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-gpu-context
    crd-gpu-context-dx12
    crd-shader-cook          # D-007 D2: the offline cook (CKIR graph -> .crdr bundle) — the DXIL run-the-cooked-bytecode gate
    Catch2::Catch2WithMain
)

# B3-e: the shared, backend-neutral CKIR raster triangle (identical graph on Vulkan + DX12).
target_include_directories(crd-gpu-context-dx12-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-shared")

crd_enable_default_pch(crd-gpu-context-dx12-tests)

catch_discover_tests(crd-gpu-context-dx12-tests DISCOVERY_MODE PRE_TEST)
