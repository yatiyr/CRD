# crd-gpu-context-cuda tests — ADR-0100 (2026-08-07, user-directed). Guarded: built only if the CUDA compute backend
# target exists (i.e. the CUDA toolkit was found), so non-NVIDIA / no-toolkit builds are unaffected.
if(NOT TARGET crd-gpu-context-cuda)
    return()
endif()

add_executable(crd-gpu-context-cuda-tests test_cuda_compute.cpp)

target_link_libraries(crd-gpu-context-cuda-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-gpu-context
    crd-gpu-context-cuda
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-gpu-context-cuda-tests)

# Shares the crd_gpu_device RESOURCE_LOCK with the other GPU-device tests: serialize under `ctest --parallel` to avoid
# cross-process driver contention.
catch_discover_tests(crd-gpu-context-cuda-tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK crd_gpu_device)
