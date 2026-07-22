# crd-shader-cook — D-007 D2 (ADR-0104): the OFFLINE shader cook.
#
# CKIR graph -> `.crdr` bundle (serialized IR + IR-reflection + per-backend blobs: SPIR-V/DXIL real bytecode via shaderc/dxc,
# CUDA/MSL/WGSL emitted source). Content-hashed. A BUILD-TIME tool — it links both GPU backends' compilers; the runtime does not.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-shader-cook STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-shader-cook
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-shader-cook
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-kir            # CKIR: emit + serialize (D1) + reflect
        crd-resources      # CRDR container (ADR-0038) — the bundle format
        crd-platform       # filesystem (content-hash cache)
        crd-jobs           # D10: parallel cook on the fiber scheduler (reuse the engine's job system, no bespoke pool)
        crd-gpu-context    # ShaderStage + ComputePipeline (D5 reload.hpp is a public header)
    PRIVATE
        crd-gpu-context-vulkan   # GLSL -> SPIR-V (shaderc)
)

# DXIL is cooked only where the D3D12 backend is built (Windows). Guarded so the cook stays portable.
if(TARGET crd-gpu-context-dx12)
    target_link_libraries(crd-shader-cook PRIVATE crd-gpu-context-dx12)
    target_compile_definitions(crd-shader-cook PRIVATE CRD_SHADERCOOK_HAS_DX12)
endif()

# Real PTX is cooked via NVRTC where the CUDA toolkit is present (mirrors the DXIL guard). Absent ⇒ CUDA is emitted as source
# only. Same CUDAToolkit_ROOT hint as crd-kir-cuda (the installer's CUDA_PATH may not be in this shell yet).
if(NOT DEFINED CUDAToolkit_ROOT AND NOT DEFINED ENV{CUDA_PATH})
    file(GLOB _crd_cuda_dirs "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v*")
    if(_crd_cuda_dirs)
        list(SORT _crd_cuda_dirs)
        list(GET _crd_cuda_dirs -1 CUDAToolkit_ROOT)
    endif()
endif()
find_package(CUDAToolkit QUIET)
if(CUDAToolkit_FOUND)
    target_link_libraries(crd-shader-cook PRIVATE CUDA::nvrtc) # CUDA C -> real PTX (portable virtual-arch bytecode)
    target_compile_definitions(crd-shader-cook PRIVATE CRD_SHADERCOOK_HAS_CUDA)
    message(STATUS "crd-shader-cook: NVRTC found -- cooking REAL PTX for the CUDA target.")
else()
    message(STATUS "crd-shader-cook: CUDAToolkit not found -- CUDA target emitted as source only (no PTX).")
endif()

target_compile_features(crd-shader-cook PUBLIC cxx_std_20)
crd_enable_default_pch(crd-shader-cook)
