# crd-kir-cuda — Phase 3.1.6 v17-c: the CKIR CUDA backend. `KirBackendCuda` over the CUDA driver API + NVRTC. A
# SEPARATE module (ADR-0096 lean-consumer): only a consumer that wants CUDA links it. NOT through crd-rhi (CUDA bypasses
# Vulkan). GUARDED on CUDAToolkit — absent (other machines / CI without CUDA) ⇒ the module is skipped, the rest of the
# engine builds unaffected. ADR-0098.

# The installer's CUDA_PATH env var may not be set in this shell yet; hint find_package with the latest installed
# toolkit under the standard Windows path.
if(NOT DEFINED CUDAToolkit_ROOT AND NOT DEFINED ENV{CUDA_PATH})
    file(GLOB _crd_cuda_dirs "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*")
    if(_crd_cuda_dirs)
        list(SORT _crd_cuda_dirs)
        list(GET _crd_cuda_dirs -1 CUDAToolkit_ROOT)
        message(STATUS "crd-kir-cuda: hinting CUDAToolkit_ROOT = ${CUDAToolkit_ROOT}")
    endif()
endif()

find_package(CUDAToolkit QUIET)
if(NOT CUDAToolkit_FOUND)
    message(STATUS "crd-kir-cuda: CUDAToolkit not found -- skipping the CUDA backend (Vulkan/CPU unaffected).")
    return()
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir-cuda STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-kir-cuda
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-kir-cuda
    PUBLIC
        crd-kir
        crd-core
        crd-containers
        crd-memory
        CUDA::cuda_driver   # the CUDA driver API (cuInit / cuMemAlloc / cuLaunchKernel …)
        CUDA::nvrtc         # runtime CUDA C -> PTX
    PRIVATE
        crd-warnings
)

target_compile_features(crd-kir-cuda PUBLIC cxx_std_20)
crd_enable_default_pch(crd-kir-cuda)
