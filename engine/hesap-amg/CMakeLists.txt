# crd-hesap-amg -- Phase 3.1.6 v4k. Algebraic multigrid as LinearOp<T>.
#
# v4k-a: Smoothed-Aggregation AMG (Vaněk 1996) — strength + aggregation +
# smoothed prolongator + Galerkin PᵀAP (via crd-hesap-sparse spgemm) + V-cycle.
# A LinearOp<T> preconditioner (M⁻¹ = one V-cycle) AND standalone solver.
# One-way dep: AMG is a LinearOp the Krylov solvers consume; never depends on
# crd-hesap-iterative. v4k-b classical Ruge-Stüben + v4l SA/AGMG land later.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-amg STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-amg
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-amg
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap         # LinearOp<T>
        crd-hesap-dense   # dense LU coarsest solve + Vector
        crd-hesap-sparse  # SparseMatrix + spgemm (Galerkin) + transpose
        crd-hesap-preconditioners  # Ilu0 smoother (convection-robust); one-way dep
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-amg PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-amg)
