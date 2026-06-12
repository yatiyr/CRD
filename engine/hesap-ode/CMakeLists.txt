# crd-hesap-ode — Phase 3.1.6 v9 (ODE/DAE cluster). ADR-0091. Two API layers: raw-span allocation-free
# stepper kernels (the eylem/animation/DAW hot-loop layer) + the general driver substrate (OdeFunction,
# controllers, dense output, integrate). Stiff members grow edges to hesap-dense (v9-d LU, v9-e complex LU)
# and hesap-direct/-iterative (v9-j) AT THOSE SLICES — v9-a keeps the edge set minimal.
#
# Edges (acyclic):
#   crd-core        — fixed-width types + asserts
#   crd-containers  — Span / Array
#   crd-memory      — IAllocator (driver scratch)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-ode STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-ode
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-ode
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-hesap-dense # v9-d: the OdeLinearSolver dense LU (Radau adds complex LU at v9-e)
        crd-hesap-sparse # v9-j: the sparse-Jacobian contract (CSR)
        crd-hesap-direct # v9-j: SparseOdeLinearSolver over the multifrontal LU
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-ode PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-ode)
