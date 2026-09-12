# crd-hesap-special — Phase 3.1.6 v12 (Statistics cluster). ADR-0094.
# Special functions + orthogonal polynomials: the reusable substrate under the distribution layer
# (crd-hesap-stats), v13 Gauss quadrature (Golub-Welsch roots/weights), and DSP.
#
# Edges (acyclic, leaf):
#   crd-core / crd-containers / crd-memory — types, Array/Span, IAllocator
#   crd-math  — crd::math::simd (the SIMD batch kernels for erf/lgamma/pdf)
#   crd-hesap — Complex<T> (v12-b complex-order Bessel; lower-layer raw per ADR-0078)
# NO edge to crd-hesap-stats.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-special STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-special
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-special
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap
        crd-jobs
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-special PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-special)
