# crd-hesap-ordering — Phase 3.1.6 v2.
#
# Fill-reducing reorderings + symbolic factorisation. The bridge from v1 sparse
# storage (crd-hesap-sparse) to v5 sparse direct solvers: a good permutation P
# (factor PAPᵀ) cuts Cholesky/LU fill-in by orders of magnitude.
#
# v2a: AdjacencyGraph + Permutation + RCM + minimal nnz(L) counter.
# v2b: AMD (approximate minimum degree) — the gate.
# v2c: full symbolic factorisation (etree + colcounts + L-pattern + supernodes).
# v2d/e: full multilevel-METIS nested dissection.
#
# Pure integer/graph + structure work on SparsePattern (no SIMD floats). Integer
# determinism: ALL tie-breaks resolve by ascending original vertex index
# (D(ord)-1); hash-like structures iterate by sorted key (D(ord)-2);
# pseudo-peripheral seed is structure-derived not RNG (D(ord)-3); adjacency is
# re-sorted ascending before use (D(ord)-4). See docs/systems/hesap-ordering.md.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-ordering STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-ordering
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-ordering
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-hesap          # CLI CommandRegistry + handle conventions
        crd-hesap-sparse   # SparsePattern (the structure orderings operate on)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-ordering PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-ordering)
