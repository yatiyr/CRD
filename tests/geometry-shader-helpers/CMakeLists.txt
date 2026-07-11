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
    crd-rhi
    crd-rhi-vulkan          # ULP-conformance test dispatches the emitted GLSL on GPU
    crd-gpu-context         # ADR-0103: ShaderStage
    crd-gpu-context-vulkan  # ADR-0103: GLSL/HLSL → SPIR-V (was crd-shader::compile_glsl/hlsl)
    crd-test-helpers        # ulp_compare + ValidationCapture
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-geometry-shader-helpers-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-geometry-shader-helpers-tests)

catch_discover_tests(crd-geometry-shader-helpers-tests)
