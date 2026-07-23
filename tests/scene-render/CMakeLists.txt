add_executable(crd-scene-render-tests
    test_scene_render.cpp     # GEO-7: chunk-grain extract + THE partial-re-upload gate (stub raster, CPU)
    test_scene_render_gpu.cpp # GEO-7: the 10k-instance cull+draw gate on a real Vulkan device ([gpu])
)

target_link_libraries(crd-scene-render-tests PRIVATE
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

catch_discover_tests(crd-scene-render-tests)
