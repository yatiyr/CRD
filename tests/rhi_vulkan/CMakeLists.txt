crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

set(RHI_VULKAN_TEST_SHADERS
    # Triangle demo shaders stay in runtime/examples/shaders/ — they're
    # real first-light renderer demos consumed by multiple test binaries
    # AND by the renderer sandbox itself.
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.vert"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.frag"
    # rhi-compute test fixtures live alongside the test binary that owns
    # them (tests/rhi_vulkan/shaders/) — they exercise crd-rhi-compute
    # substrate features (storage buffers, atomics, reflection metadata).
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_v0a.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_v0b_dispatch.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/compute_v0c_doubler.comp"
)
crd_compile_glsl(RHI_VULKAN_TEST_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders" ${RHI_VULKAN_TEST_SHADERS})

add_executable(crd-rhi-vulkan-tests ${TEST_SOURCES})

add_custom_target(crd-rhi-vulkan-test-shaders DEPENDS ${RHI_VULKAN_TEST_SPV})
add_dependencies(crd-rhi-vulkan-tests crd-rhi-vulkan-test-shaders)

target_link_libraries(crd-rhi-vulkan-tests PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-rhi
    crd-rhi-vulkan
    crd-gpu-context          # D-008 C2-b: create a VulkanGpuContext, adopt its device
    crd-gpu-context-vulkan
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-rhi-vulkan-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-rhi-vulkan-tests)

catch_discover_tests(crd-rhi-vulkan-tests)
