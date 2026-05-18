crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

# Compile the v9a-a Morton compute shader to SPIR-V next to the test
# executable so the GPU pipeline ctor can find it via
# `fs::executable_dir() / "shaders" / "compute_morton_codes.comp.spv"`
# (same pattern as tests/rhi_vulkan).
set(BVH_GPU_TEST_SHADERS
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/compute_morton_codes.comp"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/compute_morton_codes_60bit.comp"
)
crd_compile_glsl(BVH_GPU_TEST_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders" ${BVH_GPU_TEST_SHADERS})

add_executable(crd-geometry-bvh-gpu-tests ${TEST_SOURCES})

add_custom_target(crd-geometry-bvh-gpu-test-shaders DEPENDS ${BVH_GPU_TEST_SPV})
add_dependencies(crd-geometry-bvh-gpu-tests crd-geometry-bvh-gpu-test-shaders)

target_link_libraries(crd-geometry-bvh-gpu-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-units
    crd-platform
    crd-geometry-primitives
    crd-geometry-bvh
    crd-geometry-bvh-gpu
    crd-rhi
    crd-rhi-vulkan
    crd-perf
    crd-test-helpers
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-geometry-bvh-gpu-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-geometry-bvh-gpu-tests)

catch_discover_tests(crd-geometry-bvh-gpu-tests)
