# crd-sandbox-inspect-tests — DIAG.8b: the sandbox's runtime-inspection consumer, without a window.
#
# Compiles `sandbox/src/inspect_panel.cpp` directly (the showcase-tests precedent: the panel is a sandbox-internal TU)
# and drives it the way the sandbox's frame loop does. The panel loads the committed authored program through the
# renderer's app-first program seam, so the test constructs a SceneRenderer for its asset mounts only (no device).
# The engine asset root is a compiled path (a ctest ENVIRONMENT carries one variable; see the scene-render tests).
# It also compiles `sandbox/src/asset_root.cpp`, the sandbox's choice of that root, which is tested without a window,
# and (DIAG.9a) the sandbox's record sources: the frame-loop hand-over (device and host runs), the GPU device executor
# and the dialects the sandbox's diagnostic commands load programs with. (The DIAG.9b flight host it compiled became
# the engine's DiagnosticHost in DIAG.11a; its cases moved to crd-perf-tests.)

add_executable(crd-sandbox-inspect-tests
    test_inspect_panel.cpp
    test_asset_root.cpp         # the sandbox's engine asset root choice (CRD_ASSETS_DIR, else the source tree)
    test_frame_thread_calls.cpp # DIAG.9a: device records through the diagnostic panel, run on the frame-loop thread
    test_frame_thread_host.cpp  # DIAG.9a: host records through the diagnostic panel, run on the frame-loop thread
    "${CMAKE_SOURCE_DIR}/sandbox/src/inspect_panel.cpp"
    "${CMAKE_SOURCE_DIR}/sandbox/src/asset_root.cpp"
    "${CMAKE_SOURCE_DIR}/sandbox/src/frame_thread_calls.cpp"
    "${CMAKE_SOURCE_DIR}/sandbox/src/gpu_device_executor.cpp"
    "${CMAKE_SOURCE_DIR}/sandbox/src/sandbox_dialects.cpp"
)

target_include_directories(crd-sandbox-inspect-tests PRIVATE "${CMAKE_SOURCE_DIR}/sandbox/src")

target_link_libraries(crd-sandbox-inspect-tests PRIVATE
    crd-app
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-render-asset-core
    crd-scene-render
    crd-ceir
    crd-ceir-cook
    crd-ceir-gpu           # DIAG.9a: run_device_block under the sandbox's GPU device executor
    crd-ceir-host          # DIAG.9a: the host provider's executor, which the sandbox hands to the frame loop
    crd-jobs               # DIAG.9a: the pool (and frame arenas) a host run needs
    crd-gpu-context-vulkan # DIAG.9a: the executor's Vulkan compute context (the device case soft-skips without one)
    crd-kir                # DIAG.9a: the device fixture's kernels
    crd-perf               # DIAG.9a: the diagnostic service the replay commands register with
    crd-perf-ui            # DIAG.9a: the diagnostic panel whose worker thread hands device runs to the frame loop
    Catch2::Catch2WithMain
)
if(WIN32)
    target_link_libraries(crd-sandbox-inspect-tests PRIVATE crd-gpu-context-dx12) # DIAG.9a: the DX12 executor
endif()

target_compile_definitions(crd-sandbox-inspect-tests PRIVATE
    CRD_SANDBOX_INSPECT_ENGINE_ASSETS="${CMAKE_SOURCE_DIR}/assets")

crd_enable_default_pch(crd-sandbox-inspect-tests)
# DIAG.9a: the device cases open a GPU device, so the exe shares the GPU suites' lock.
crd_discover_tests(crd-sandbox-inspect-tests PROPERTIES RESOURCE_LOCK crd_gpu_device)
