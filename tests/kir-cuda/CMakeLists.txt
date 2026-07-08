# crd-kir-cuda tests — Phase 3.1.6 v17-c. Guarded: only built if the CUDA backend target exists (CUDAToolkit present).
if(NOT TARGET crd-kir-cuda)
    return()
endif()

add_executable(crd-kir-cuda-tests
    test_backend_cuda.cpp
)

target_link_libraries(crd-kir-cuda-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-cuda
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-cuda-tests)

# PRE_TEST: enumerate at ctest time (not build time) — the exe needs the CUDA nvrtc DLL on PATH to start, which a
# build-time discovery run doesn't have.
catch_discover_tests(crd-kir-cuda-tests DISCOVERY_MODE PRE_TEST)
