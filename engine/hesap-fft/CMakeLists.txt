# crd-hesap-fft — Phase 3.1.6 v10 (FFT cluster). New ADR. Gold standards FFTW3 + PocketFFT + MKL; ratified
# bar = BEAT MKL, scope = a→h (memory project_v10_fft_plan; plan in docs/phases/phase-3.1.6-hesap.md).
# v10-a: complex FFT substrate — deterministic plan-from-factorization (no runtime measurement), precomputed
# twiddle table, radix-2 (the correct baseline + the contract); Stockham radix-8 SIMD codelets = v10-b.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math  — crd::math::simd (the SIMD kernels land at v10-b)
#   crd-hesap — Complex<T> (FFT is inherently complex; lower-layer raw per ADR-0078)

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
crd_enable_default_pch(crd-hesap-fft)
