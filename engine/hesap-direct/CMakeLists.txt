# crd-hesap-direct — Phase 3.1.6 v5. Sparse DIRECT factorization.
#
# v5a-1: module scaffold + IFactorization<T> (factor-once / solve-many, multi-RHS
#        from day 1) + Frontal<T> / extend_add (the multifrontal assembly kernel,
#        unit-tested standalone; consumed by v5c QR / v5d LDLᵀ / v5e HSS).
#
# The flagship left-looking supernodal Cholesky (v5a-1..3) + sparse LU (v5b) +
# multifrontal QR (v5c) + LDLᵀ (v5d) + rank-structured HSS/BLR (v5e) land next.
# Consumes v2c SymbolicFactor (crd-hesap-ordering) + v0 dense panel kernels
# (crd-hesap-dense) + the task DAG (crd-hesap-sched). Cross-thread bit-determinism
# moat held per family. ADR-0065 §27.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-direct STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-direct
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-direct
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap
        crd-hesap-sparse
        crd-hesap-ordering
        crd-hesap-dense
        crd-jobs
        crd-hesap-sched
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-direct PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-direct)
