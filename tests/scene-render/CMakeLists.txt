add_executable(crd-scene-render-tests
    test_scene_render.cpp     # GEO-7: chunk-grain extract + THE partial-re-upload gate (stub raster, CPU)
    test_scene_render_gpu.cpp # GEO-7: the 10k-instance cull+draw gate on a real Vulkan device ([gpu])
    test_csm.cpp              # REN-3.2-b: CPU cascade fitting + the no-swim stabilization gates
)

target_link_libraries(crd-scene-render-tests PRIVATE
    crd-frame-cook   # REN-36: the AUTHORED graph asset the CSM gate parses
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-units
    crd-platform
    crd-geometry-primitives
    crd-geometry-spatial
    crd-scene
    crd-resources
    crd-scene-render
    crd-gpu-context
    crd-gpu-context-vulkan
    crd-kir
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-render-tests)
target_compile_definitions(crd-scene-render-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # std::getenv (the drift gate's ctest env)
target_link_libraries(crd-scene-render-tests PRIVATE crd-material-cook crd-vertex-cook crd-light-cook) # REN-38: the drift gate parses all five vocabularies
if(WIN32) # REN-38-F6: the DX12 twins of the advanced-family render gates (the backend exists only on Windows)
    target_link_libraries(crd-scene-render-tests PRIVATE crd-gpu-context-dx12)
endif()

# REN-38 audit: the DRIFT GATE reads the shipped authored assets — the root rides ctest, so the guard lives in
# ctest exactly like every cross-config guard (binary-direct runs SKIP rather than passing on nothing).
catch_discover_tests(crd-scene-render-tests
    PROPERTIES ENVIRONMENT "CRD_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets")
