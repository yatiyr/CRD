# crd-hesap-sparse — Phase 3.1.6 v1a.
#
# Sparse matrix substrate. Goal: beat Eigen SparseCore on spmv + spgemm.
#
# v1a-1: pattern/values/analysis trinity + SparseId + SparseFormat +
#        topology_hash (FNV-1a-64) + 'HSPM' CRDR pin + determinism spec.
#        STATIC (sparse_pattern.cpp ships the topology hash).
# v1a-2: COO TripletBuilder -> CSR compress() + per-op CLI registration.
# v1a-3: CSC + structural queries + smoke + vs-Eigen assembly bench + close.
#
# Determinism (ADR-0063): topology_hash is little-endian explicit-byte-feed
# (endian-independent); future row-parallel spmv uses disjoint-row ownership;
# spgemm numeric output is canonical column-sorted. See docs/systems/hesap-sparse.md.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-sparse STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-sparse
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-sparse
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math       # Complex<T> values (c32/c64) + future SELL SIMD (v1b)
        crd-hesap      # SparseId sibling to MatrixId/VectorId; 'HSPM' CRDR pin
        crd-jobs       # future row-balanced parallel spmv/spgemm (v1b+)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-sparse PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-sparse)
