crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

# The crd-geometry-bvh-gpu module ships its compute shaders alongside the
# module (engine/geometry-bvh-gpu/shaders/) — they're engine source assets,
# not examples. Tests compile them to SPIR-V next to the test executable so
# the GPU pipeline ctor finds them via `fs::executable_dir() / "shaders"`.
set(BVH_GPU_SHADERS_DIR "${CMAKE_SOURCE_DIR}/engine/geometry-bvh-gpu/shaders")
set(BVH_GPU_TEST_SHADERS
    "${BVH_GPU_SHADERS_DIR}/compute_morton_codes.comp"
    "${BVH_GPU_SHADERS_DIR}/compute_morton_codes_60bit.comp"
    "${BVH_GPU_SHADERS_DIR}/radix_sort_init.comp"
    "${BVH_GPU_SHADERS_DIR}/radix_sort_histogram.comp"
    "${BVH_GPU_SHADERS_DIR}/radix_sort_scan.comp"
    "${BVH_GPU_SHADERS_DIR}/radix_sort_scatter.comp"
    "${BVH_GPU_SHADERS_DIR}/lbvh_fat_build.comp"
    "${BVH_GPU_SHADERS_DIR}/lbvh_fat_upsweep.comp"
    "${BVH_GPU_SHADERS_DIR}/lbvh_fat_init_leaves.comp"
    "${BVH_GPU_SHADERS_DIR}/lbvh_fat_merge_level.comp"
    "${BVH_GPU_SHADERS_DIR}/lbvh_fat_extract_prim_indices.comp"
    "${BVH_GPU_SHADERS_DIR}/lbvh_fat_upsweep_persistent.comp"
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
    crd-jobs                # v9a-b1-parallel listener uses jobs::init/shutdown
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
