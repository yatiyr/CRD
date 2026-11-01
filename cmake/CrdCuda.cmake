# CrdCuda.cmake — the host's CUDA Toolkit for what uses it: crd-kir-cuda, crd-gpu-context-cuda, crd-shader-cook's PTX
# cooking (each skips itself without a toolkit) and the GPU boards' CUDA peer programs (bench/, CRD_BUILD_GPU_VS_CUDA).

# crd_find_cuda_toolkit([find_package arguments ...]): find_package(CUDAToolkit) in the calling directory, its imported
# targets visible there. A Windows installer's CUDA_PATH does not reach a shell started before the install, so without
# CUDAToolkit_ROOT or CUDA_PATH the newest toolkit under the installer's directory is the hint (versions compared as
# numbers, v13.3 after v9.2).
macro(crd_find_cuda_toolkit)
    if(NOT DEFINED CUDAToolkit_ROOT AND NOT DEFINED ENV{CUDA_PATH})
        file(GLOB _crd_cuda_dirs "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*")
        if(_crd_cuda_dirs)
            list(SORT _crd_cuda_dirs COMPARE NATURAL)
            list(GET _crd_cuda_dirs -1 CUDAToolkit_ROOT)
            message(STATUS "CUDA Toolkit hint: CUDAToolkit_ROOT = ${CUDAToolkit_ROOT}")
        endif()
    endif()
    find_package(CUDAToolkit ${ARGN})
endmacro()
