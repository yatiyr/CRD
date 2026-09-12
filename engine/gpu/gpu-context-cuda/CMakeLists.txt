# crd-gpu-context-cuda — ADR-0100 (user-directed 2026-08-07): the CUDA implementation of crd::gpu::IComputeContext, a
# THIRD compute backend behind the same dispatch surface as Vulkan + DX12. CUDA driver API + NVRTC (CUDA C -> CUBIN).
# GUARDED on CUDAToolkit — absent (non-NVIDIA / CI without CUDA) ⇒ the module self-skips and the rest of the engine
# builds unaffected. Shares the device primary context with crd-kir-cuda (no duplicate CUDA init).

# The installer's CUDA_PATH may not be set in this shell; hint find_package with the latest toolkit under the std path.
if(NOT DEFINED CUDAToolkit_ROOT AND NOT DEFINED ENV{CUDA_PATH})
    file(GLOB _crd_cuda_dirs "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*")
    if(_crd_cuda_dirs)
        list(SORT _crd_cuda_dirs)
        list(GET _crd_cuda_dirs -1 CUDAToolkit_ROOT)
        message(STATUS "crd-gpu-context-cuda: hinting CUDAToolkit_ROOT = ${CUDAToolkit_ROOT}")
    endif()
endif()

find_package(CUDAToolkit QUIET)
if(NOT CUDAToolkit_FOUND)
    message(STATUS "crd-gpu-context-cuda: CUDAToolkit not found -- skipping the CUDA compute backend (Vulkan/DX12 unaffected).")
    return()
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-gpu-context-cuda STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-gpu-context-cuda
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-gpu-context-cuda
    PUBLIC
        crd-gpu-context     # the IComputeContext interface (compute.hpp)
        crd-containers
        CUDA::cuda_driver   # the CUDA driver API (cuInit / cuMemAlloc / cuLaunchKernel / cuEvent…)
        CUDA::nvrtc         # runtime CUDA C -> CUBIN
    PRIVATE
        crd-core
        crd-memory
)

target_compile_features(crd-gpu-context-cuda PUBLIC cxx_std_20)
crd_enable_default_pch(crd-gpu-context-cuda)
