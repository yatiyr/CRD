# crd-hesap-eigen -- Phase 3.1.6 v6. SPARSE EIGENVALUE, matrix-free over the LinearOp<T>.
#
# Rayleigh-Ritz on the small projected problem reuses the dense eigensolvers (crd-hesap-dense eig_sym /
# eig_nonsym / svd). Header-only templates; the .cpp carries the anchor (+ the hesap.eigen.* CLI at v6-z).
# Determinism moat: deterministic counter-RNG start + fixed-order reorthogonalization + pinned sign/order
# convention ⇒ eigenpairs bit-identical across {1,2,4,8} workers (the differentiator). ADR-0065 §v6.
#
# Edges (acyclic; eigen is high in the hesap stack): hesap-dense (Rayleigh-Ritz), hesap-sparse (SparseLinearOp).
# Later slices add hesap-direct (shift-invert, v6-d), hesap-preconditioners + hesap-iterative (LOBPCG/JD, v6-e/f).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-eigen STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-eigen
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-eigen
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap          # LinearOp<T>
        crd-hesap-dense    # eig_sym / eig_nonsym Rayleigh-Ritz + Vector/Matrix/Symmetric + blas1
        crd-hesap-sparse   # SparseLinearOp<T>
        crd-hesap-direct   # v6-d shift-invert: the v5 multifrontal LU factor of (A − σI)
        crd-jobs           # parallel spmv (the only parallel step; the moat keeps it bit-exact)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-eigen PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-eigen)
