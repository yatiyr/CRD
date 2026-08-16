# crd-gpu-context-dx12 tests — Phase 3.1.6 v17-i (ADR-0100). Guarded: only built if the D3D12 compute backend target
# exists (Windows), so Linux/macOS builds are unaffected.
if(NOT TARGET crd-gpu-context-dx12)
    return()
endif()

add_executable(crd-gpu-context-dx12-tests
    test_dx12_compute.cpp
    test_dx12_frame_graph.cpp # REN-1 pt-2 (D-007 row 98): the DX12 frame graph — one submission + transient aliasing
    test_dx12_gsplat.cpp
    test_dx12_raster.cpp
    test_dx12_rt.cpp
    ../gpu-shared/win32_test_window.cpp # RET-2: the isolated <windows.h> TU — a REAL window for the present gates
)

target_link_libraries(crd-gpu-context-dx12-tests PRIVATE
    crd-frame-cook   # REN-36.2: the cooked frame-graph asset + its runtime
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-gpu-context
    crd-gpu-context-dx12
    crd-vertex-cook # REN-38-F13: the authored RT stages the F13 gate cooks
    crd-shader-cook          # D-007 D2: the offline cook (CKIR graph -> .crdr bundle) — the DXIL run-the-cooked-bytecode gate
    crd-ceir                 # CEIR-19c: build the ceir.rt program (blas/tlas/ray_query) the bridge lowers
    crd-ceir-gpu             # CEIR-19c: lower_region + execute_rt_lowered (the ceir.rt->gpu execution bridge under test)
    Catch2::Catch2WithMain
)

# B3-e: the shared, backend-neutral CKIR raster triangle (identical graph on Vulkan + DX12).
target_include_directories(crd-gpu-context-dx12-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-shared")

crd_enable_default_pch(crd-gpu-context-dx12-tests)

# Shares the crd_gpu_device RESOURCE_LOCK with the Vulkan GPU tests (see that CMakeLists): GPU-device tests
# serialize under `ctest --parallel` to avoid cross-process driver/cook contention, non-GPU tests still parallelize.
# CEIR-18a-1: the repo root, so the shared cull gate loads assets/compute/scene_light_cull.ckir (the retired builder's asset).
target_compile_definitions(crd-gpu-context-dx12-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

catch_discover_tests(crd-gpu-context-dx12-tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK crd_gpu_device)
