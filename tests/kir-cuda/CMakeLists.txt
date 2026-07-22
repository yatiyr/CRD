# crd-kir-cuda tests — Phase 3.1.6 v17-c. Guarded: only built if the CUDA backend target exists (CUDAToolkit present).
if(NOT TARGET crd-kir-cuda)
    return()
endif()

add_executable(crd-kir-cuda-tests
    test_backend_cuda.cpp
    test_autotune_cuda.cpp
    test_autotune_cublas.cpp
)

target_link_libraries(crd-kir-cuda-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-cuda
    Catch2::Catch2WithMain
)

# ADR-0098 §4 · AS-4: the cuBLAS vendor board (CKIR-autotuned GEMM vs cuBLAS Sgemm, matched f32). Linked only where cuBLAS is
# present (it ships with the CUDA toolkit we already found); absent ⇒ the board test compiles out gracefully.
# find_package targets are directory-scoped, so re-find CUDAToolkit here to expose CUDA::cublas in this scope (cached, cheap).
find_package(CUDAToolkit QUIET)
if(TARGET CUDA::cublas)
    target_link_libraries(crd-kir-cuda-tests PRIVATE CUDA::cublas CUDA::cudart)
    target_compile_definitions(crd-kir-cuda-tests PRIVATE CRD_HAS_CUBLAS)
    message(STATUS "crd-kir-cuda-tests: cuBLAS found -- the AS-4 vendor board is enabled.")
endif()

crd_enable_default_pch(crd-kir-cuda-tests)

# PRE_TEST: enumerate at ctest time (not build time) — the exe needs the CUDA nvrtc DLL on PATH to start, which a
# build-time discovery run doesn't have.
catch_discover_tests(crd-kir-cuda-tests DISCOVERY_MODE PRE_TEST)
