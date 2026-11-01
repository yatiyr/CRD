# The GPU compute and FFT boards' CUDA programs (CRD_BUILD_GPU_VS_CUDA; local only on Linux, never in CI or a shipped
# build): the vendor peers (cuFFT, CUB, cuBLASLt), the CUDA duels of the CKIR-emitted kernels and the Nsight Compute
# targets, each with its board (docs/bench/2026-07-13-gpu-fft-cufft-gold.md, 2026-07-13-gpu-compute-primitives.md,
# 2026-07-14-fused-mlp-cublas-gold.md, 2026-07-15-fft-ocean-batched-ifft.md, 2026-07-23-as4-flash-attention-fusion.md).
# nvcc compiles them for CMAKE_CUDA_ARCHITECTURES (default 89, the measuring PC's RTX 4070 Ti SUPER) with the tree's
# C++ compiler as its host compiler, against the toolkit crd_find_cuda_toolkit finds (CUDAToolkit_ROOT: the pinned one
# from scripts/install-cuda.py). The cuFFT, CUB, CKIR FFT and attention programs exit 1 at their first failed CUDA or
# cuFFT call (cuda_bench.hpp's CRD_CUDA_CHECK); the MLP and onesweep programs still check through their own helpers
# (REPO.3). They link no Cerid library, so the ISA isolation guard leaves them out. The list of built
# programs goes to <tree>/bench/cuda-programs.txt, which scripts/run_bench_gpu_cuda.sh runs.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "[gpu-vs-cuda] the CUDA programs build on Linux (a Windows NVIDIA host runs them under WSL2)")
endif()
crd_find_cuda_toolkit(REQUIRED)
if(NOT CMAKE_CUDA_COMPILER)
    set(CMAKE_CUDA_COMPILER "${CUDAToolkit_NVCC_EXECUTABLE}")
endif()
set(CMAKE_CUDA_HOST_COMPILER "${CMAKE_CXX_COMPILER}")
if(NOT CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 89)
endif()
enable_language(CUDA)

# nvcc's own diagnostics as errors, and the host compiler's under the tree's warning set (crd-warnings' GCC and Clang
# flags; the toolkit's headers are system headers to it) but -Wpedantic: the host compiler reads nvcc's translation of
# each unit, whose GCC-style `# <line> "<file>"` directives it reports on every line under -Wpedantic.
set(crd_cuda_host_warnings -Wall -Wextra -Wconversion -Wshadow -Wdouble-promotion -Wnull-dereference -Wformat=2)
if(CRD_WARNINGS_AS_ERRORS)
    list(APPEND crd_cuda_host_warnings -Werror)
endif()
list(JOIN crd_cuda_host_warnings "," crd_cuda_host_warnings)
set(crd_cuda_warnings "SHELL:-Xcompiler ${crd_cuda_host_warnings}")
if(CRD_WARNINGS_AS_ERRORS)
    list(APPEND crd_cuda_warnings "SHELL:--Werror all-warnings")
endif()

# crd_add_cuda_program(<name> <source> [LIBRARIES <target> ...] [OPTIONS <nvcc option> ...])
set(crd_cuda_programs "")
function(crd_add_cuda_program name source)
    cmake_parse_arguments(PARSE_ARGV 2 program "" "" "LIBRARIES;OPTIONS")
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options(${name} PRIVATE ${crd_cuda_warnings} ${program_OPTIONS})
    target_link_libraries(${name} PRIVATE ${program_LIBRARIES})
    # nvcc's default runtime, the static cudart the boards' binaries had. The toolkit's library directory is one of
    # the CUDA compiler's implicit link directories, which CMake leaves out of the run path, so the shared cuFFT and
    # cuBLASLt are found through BUILD_RPATH.
    set_target_properties(${name} PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON CUDA_EXTENSIONS OFF
                          CUDA_RUNTIME_LIBRARY Static BUILD_RPATH "${CUDAToolkit_LIBRARY_DIR}"
                          INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)
    set(crd_cuda_programs ${crd_cuda_programs} ${name} PARENT_SCOPE)
endfunction()

crd_add_cuda_program(ckir_mlp_bench gpu-compute/ckir_mlp_bench.cu LIBRARIES CUDA::cublasLt)
crd_add_cuda_program(ckir_mlp_bwd_bench gpu-compute/ckir_mlp_bwd_bench.cu LIBRARIES CUDA::cublasLt)
crd_add_cuda_program(ckir_onesweep_bench gpu-compute/ckir_onesweep_bench.cu)
crd_add_cuda_program(cub_radixsort_bench gpu-compute/cub_radixsort_bench.cu)
crd_add_cuda_program(cub_reduce_bench gpu-compute/cub_reduce_bench.cu)
crd_add_cuda_program(cub_scan_bench gpu-compute/cub_scan_bench.cu)
crd_add_cuda_program(flash_attention_bench gpu-compute/flash_attention_bench.cu)
crd_add_cuda_program(mlp_backward_bench gpu-compute/mlp_backward_bench.cu LIBRARIES CUDA::cublasLt)
crd_add_cuda_program(mlp_fused_bench gpu-compute/mlp_fused_bench.cu LIBRARIES CUDA::cublasLt)
# The CKIR kernels' `precise` semantics: no FMA contraction, as the Vulkan and NVRTC paths compile them.
crd_add_cuda_program(ckir_fft_profilee gpu-fft/ckir_fft_profilee.cu OPTIONS --fmad=false)
crd_add_cuda_program(cufft_1024_profilee gpu-fft/cufft_1024_profilee.cu LIBRARIES CUDA::cufft)
crd_add_cuda_program(cufft_2d_c2c_batched_bench gpu-fft/cufft_2d_c2c_batched_bench.cu LIBRARIES CUDA::cufft)
crd_add_cuda_program(cufft_2d_conv_batched_bench gpu-fft/cufft_2d_conv_batched_bench.cu LIBRARIES CUDA::cufft)
crd_add_cuda_program(cufft_2d_conv_bench gpu-fft/cufft_2d_conv_bench.cu LIBRARIES CUDA::cufft)
crd_add_cuda_program(cufft_2d_conv_r2c_batched_bench gpu-fft/cufft_2d_conv_r2c_batched_bench.cu
                     LIBRARIES CUDA::cufft)
crd_add_cuda_program(cufft_bench gpu-fft/cufft_bench.cu LIBRARIES CUDA::cufft)
crd_add_cuda_program(cufft_conv_bench gpu-fft/cufft_conv_bench.cu LIBRARIES CUDA::cufft)

add_custom_target(crd-gpu-vs-cuda DEPENDS ${crd_cuda_programs})
list(TRANSFORM crd_cuda_programs REPLACE "(.+)" "$<TARGET_FILE:\\1>")
list(JOIN crd_cuda_programs "\n" crd_cuda_program_files)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cuda-programs.txt" CONTENT "${crd_cuda_program_files}\n")
message(STATUS "[gpu-vs-cuda] CUDA ${CUDAToolkit_VERSION} (${CMAKE_CUDA_COMPILER}), architectures "
               "${CMAKE_CUDA_ARCHITECTURES}, host compiler ${CMAKE_CUDA_HOST_COMPILER}")
