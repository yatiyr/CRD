# crd-hesap-fft — Phase 3.1.6 v10 (FFT cluster). New ADR. Scope = a→h (memory project_v10_fft_plan; plan in
# docs/phases/phase-3.1.6-hesap.md; performance scoreboard + reference comparisons in docs/research/).
# v10-a: complex FFT substrate — deterministic plan-from-factorization (no runtime measurement), precomputed
# twiddle table, radix-2 (the correct baseline + the contract); Stockham radix-8 SIMD codelets = v10-b.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math  — crd::math::simd (the SIMD kernels land at v10-b)
#   crd-hesap — Complex<T> (FFT is inherently complex; lower-layer raw per ADR-0078)
#   crd-jobs  — the batched transforms' jobs (detail/batch_arms.hpp: a range of transforms a job, a plan each)

if(CRD_HESAP_FFT_PROFILE)
    # Directory scope, so the archive and the kernel units below count the same phases; INTERFACE below for consumers.
    add_compile_definitions(CRD_FFT_PROFILE CRD_NUFFT_PROFILE)
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-fft STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-fft
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-fft
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap
        crd-jobs       # detail/batch_arms.hpp: the batched transforms' jobs
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-fft PUBLIC cxx_std_20)
if(CRD_HESAP_FFT_PROFILE)
    target_compile_definitions(crd-hesap-fft INTERFACE CRD_FFT_PROFILE CRD_NUFFT_PROFILE)
endif()
crd_enable_default_pch(crd-hesap-fft)

# The transform engine, one body per CPU level (ADR-0134); src/fft_kernel_dispatch.cpp lists them. The generated
# codelets it calls sit in their own units, balanced by line count so no unit outgrows a compiler (gen_fft_batched.py
# writes fft_batched_codelets_<k>.cpp, gen_fft_codelets.py fft_twiddle_codelets_<k>.cpp; fft_codegen_split.py moved the
# hierarchical ones).
crd_add_cpu_kernels(crd-hesap-fft NAME fft LEVELS portable sse2 avx2 avx512 INCLUDES crd-math crd-hesap
                    SOURCES
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_kernels.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_0.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_1.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_2.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_3.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_4.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_5.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_batched_codelets_6.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_hier_codelets_0.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_hier_codelets_1.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_twiddle_codelets_0.cpp"
                        "${CMAKE_CURRENT_SOURCE_DIR}/kernels/fft_twiddle_codelets_1.cpp")
# The codelet units are the tree's largest compiles on every level: a batched unit peaked at 4.1 GiB at sse2 under
# GCC 13 -O2 -g (avx512 1.7, avx2 0.7, portable 0.3) and at 4.2 GiB at avx512 under MSVC /O2 (avx2 2.2), 2026-10-30.
# Six sse2 units at once starved a 16 GB hosted runner until it was lost (run 41059057789), so a pool runs as many as
# the host's memory holds beside the rest of the build: the build board's 4 GiB reserve, then one unit per 4.2 GiB
# (docs/design/build-performance.md#measured-board-and-decisions).
if(CMAKE_GENERATOR MATCHES "Ninja")
    cmake_host_system_information(RESULT _crd_fft_memory_mib QUERY TOTAL_PHYSICAL_MEMORY)
    math(EXPR _crd_fft_codelet_jobs "(${_crd_fft_memory_mib} - 4096) * 10 / 43008")
    if(_crd_fft_codelet_jobs LESS 1)
        set(_crd_fft_codelet_jobs 1)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS crd_fft_codelets=${_crd_fft_codelet_jobs})
    foreach(level IN ITEMS portable sse2 avx2 avx512)
        if(TARGET crd-hesap-fft-fft-${level})
            set_property(TARGET crd-hesap-fft-fft-${level} PROPERTY JOB_POOL_COMPILE crd_fft_codelets)
        endif()
    endforeach()
    unset(_crd_fft_codelet_jobs)
    unset(_crd_fft_memory_mib)
endif()
