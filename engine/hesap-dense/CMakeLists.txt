# crd-hesap-dense — Phase 3.1.6 v0b.
#
# Dense matrix substrate.
# v0a: matrix-type catalog HEADERS as empty shells (INTERFACE library).
# v0b: Vector<T> + BLAS L1 across 4 type variants (f32/f64/Complex32/Complex64) +
#      Kahan-Babuška-Neumaier compensated pairwise summation tree + per-op CLI
#      command registration. Flips from INTERFACE → STATIC since blas1.cpp +
#      cli_register.cpp arrive in this slice.
#
# BLAS L1 ops:
#   axpy / dot / dotu / dotc / nrm2 / scal / copy / swap / asum / iamax
#
# Determinism (ADR-0063): all sums route through detail::pairwise_sum_produced
# (KBN-compensated pairwise tree; kPairwiseLeafBlock = 8 = Vec8f lane count).
# Bit-exact across SIMD widths AS LONG AS each backend walks the same tree.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-dense STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-dense
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-dense
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math       # v0d: gemm AVX2 microkernel uses crd-math::simd::Vec8f/Vec4d
        crd-hesap
        crd-jobs       # v0d-parallelism: gemm_parallel uses crd::jobs::parallel_for
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-dense PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-dense)
