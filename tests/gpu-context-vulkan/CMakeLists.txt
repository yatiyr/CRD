# crd-gpu-context-vulkan tests — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own.
# Also hosts the CKIR GLSL-emitter compile gate (ADR-0103 / D-008): the shaderc compiler moved here from crd-shader, so
# the "emitted GLSL compiles to SPIR-V" test lives with it (and off the crd-kir link-isolation smoke, which must stay
# GPU-API-free).
add_executable(crd-gpu-context-vulkan-tests
    test_vulkan_context.cpp
    test_ckir_glsl_compile.cpp
    test_vulkan_rt.cpp
    test_vulkan_hair_showcase.cpp
    test_vulkan_hair_swatch_rt.cpp
    test_vulkan_gsplat.cpp
    ../gpu-shared/win32_test_window.cpp # RET-2: the isolated <windows.h> TU — a REAL window for the present gates
)

target_link_libraries(crd-gpu-context-vulkan-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir                  # the CKIR IR + GLSL emitter under test
    crd-gpu-context
    crd-gpu-context-vulkan
    crd-shader-cook          # D-007 D2: the offline cook (CKIR graph -> .crdr bundle) under test
    crd-jobs                 # D-007 D10: the fiber scheduler for the parallel-cook test (jobs::init/shutdown)
    crd-cooker               # GEO-1: the wave1 (.stl/.obj/.ply) asset cook handler (the import→cook→draw gate)
    crd-imgui                # RET-5: ImGuiGpuBackend under test (the overlay-present gate)
    Catch2::Catch2WithMain
)

# B3-e: the shared, backend-neutral CKIR raster triangle (the identical graph the DX12 suite draws).
target_include_directories(crd-gpu-context-vulkan-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-shared")
# RET-5: Dear ImGui headers for the overlay-present gate (same SYSTEM-private exposure crd-imgui's consumers use).
target_include_directories(crd-gpu-context-vulkan-tests SYSTEM PRIVATE "${imgui_SOURCE_DIR}")

crd_enable_default_pch(crd-gpu-context-vulkan-tests)

catch_discover_tests(crd-gpu-context-vulkan-tests)
