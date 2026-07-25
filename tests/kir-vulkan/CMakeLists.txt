# crd-kir-vulkan tests — Phase 3.1.6 v17-b: the first CKIR kernel on the GPU + the GPU↔CPU conformance gate.
add_executable(crd-kir-vulkan-tests
    test_backend_vulkan.cpp
)

target_link_libraries(crd-kir-vulkan-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-vulkan
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-vulkan-tests)

# Shares the crd_gpu_device RESOURCE_LOCK (see tests/gpu-context-vulkan): GPU-device tests serialize under
# `ctest --parallel` to avoid cross-process driver/cook contention.
catch_discover_tests(crd-kir-vulkan-tests PROPERTIES RESOURCE_LOCK crd_gpu_device)
