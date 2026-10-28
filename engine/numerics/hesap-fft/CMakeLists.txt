# crd-hesap-fft — Phase 3.1.6 v10 (FFT cluster). New ADR. Scope = a→h (memory project_v10_fft_plan; plan in
# docs/phases/phase-3.1.6-hesap.md; performance scoreboard + reference comparisons in docs/research/).
# v10-a: complex FFT substrate — deterministic plan-from-factorization (no runtime measurement), precomputed
# twiddle table, radix-2 (the correct baseline + the contract); Stockham radix-8 SIMD codelets = v10-b.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math  — crd::math::simd (the SIMD kernels land at v10-b)
#   crd-hesap — Complex<T> (FFT is inherently complex; lower-layer raw per ADR-0078)

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
