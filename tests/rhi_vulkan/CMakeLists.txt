crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

set(RHI_VULKAN_TEST_SHADERS
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.vert"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.frag"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/compute_v0a.comp"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/compute_v0b_dispatch.comp"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/compute_v0c_doubler.comp"
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
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-rhi-vulkan-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

crd_enable_default_pch(crd-rhi-vulkan-tests)

catch_discover_tests(crd-rhi-vulkan-tests)
