# crd-gpu-context-vulkan tests — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own.
add_executable(crd-gpu-context-vulkan-tests
    test_vulkan_context.cpp
)

target_link_libraries(crd-gpu-context-vulkan-tests PRIVATE
    crd-core
    crd-gpu-context
    crd-gpu-context-vulkan
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-gpu-context-vulkan-tests)

catch_discover_tests(crd-gpu-context-vulkan-tests)
