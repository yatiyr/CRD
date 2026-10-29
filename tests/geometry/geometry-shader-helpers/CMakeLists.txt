crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-geometry-shader-helpers-tests ${TEST_SOURCES})

target_link_libraries(crd-geometry-shader-helpers-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-platform
    crd-geometry-primitives
    crd-geometry-shader-helpers
    crd-gpu-context         # ADR-0103: ShaderStage + IComputeContext (RET-7: the ULP dispatch runs HERE, rhi is gone)
    crd-gpu-context-vulkan  # ADR-0103: GLSL/HLSL → SPIR-V + VulkanComputeContext
    crd-test-helpers        # ulp_compare + ValidationCapture
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-shader-helpers-tests)

crd_discover_tests(crd-geometry-shader-helpers-tests PROPERTIES RESOURCE_LOCK crd_gpu_device)
