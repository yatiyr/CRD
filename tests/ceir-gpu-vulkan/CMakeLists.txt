# CEIR-13z-1b (Vulkan): a CEIR compute ASSET lowered (13d) + executed on a real Vulkan device via execute_lowered (ADR-0126),
# proven byte-identical to the direct CKIR dispatch. Per-backend (mirrors kir-vulkan) — a dx12-linked target can't build on
# Linux, so keeping Vulkan separate preserves the WSL llvmpipe run. The device case soft-skips with no adapter; the
# device-free lower-shape case always runs (the all-skip false-green guard).
add_executable(crd-ceir-gpu-vulkan-tests
    test_ceir_add_vulkan.cpp
)

target_link_libraries(crd-ceir-gpu-vulkan-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-ceir          # the IR + the asset builder
    crd-ceir-gpu      # lower_region + execute_lowered (ADR-0125/0126)
    crd-kir           # the CKIR add kernel + the CPU dispatch harness
    crd-kir-vulkan    # the Vulkan backend (transitively: gpu-context-vulkan — VulkanComputeContext, compile_glsl_to_spirv, ValidationCapture)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-gpu-vulkan-tests)
catch_discover_tests(crd-ceir-gpu-vulkan-tests PROPERTIES RESOURCE_LOCK crd_gpu_device)
