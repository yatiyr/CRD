# crd-hesap-stats — Phase 3.1.6 (v12 cluster, created early as a v12-pull for v7-i). STATISTICS substrate:
# the Philox4x32-10 counter-based RNG (reproducible minibatch sampling / replay / the v12 stats cluster).
#
# Edges (acyclic; deliberately MINIMAL — the RNG is a leaf primitive):
#   crd-core        — fixed-width types + asserts
#   crd-containers  — Span (the shuffle primitive)
#   crd-math        — the kernel dispatch (crd/math/kernel_dispatch.hpp) and the scalar math of the distributions

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-stats STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-stats
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-stats
    PUBLIC
        crd-core
        crd-containers
        crd-math            # SIMD-DISPATCH: the stats kernel table; the distributions' crd::math scalar functions
        crd-memory          # v12-f: IAllocator (AliasTable)
        crd-hesap-special   # v12-f: lgamma (Poisson PTRS) + CDFs for distribution layer; acyclic (special is a leaf)
        crd-hesap-dense     # v12-k: factor_cholesky for the multivariate distributions (SANITY rule 8; acyclic — dense never references stats)
        crd-hesap-quadrature # v12-n: gauss_hermite/laguerre for the studentized range (Tukey HSD); acyclic
        crd-jobs            # v12-o: parallel resampling (resampling_parallel.hpp); acyclic (jobs = core+containers)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-stats PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-stats)

# The Philox and Threefry bulk fills and the Student-t score reduction, one body per CPU level (ADR-0134);
# src/stats_kernel_dispatch.cpp lists them.
crd_add_cpu_kernels(crd-hesap-stats NAME stats LEVELS portable sse2 avx2 INCLUDES crd-math
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/stats_kernels.cpp")
