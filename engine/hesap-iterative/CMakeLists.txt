# crd-hesap-iterative -- Phase 3.1.6 v4a.
#
# Krylov iterative solvers over the matrix-free LinearOp<T>. v4a: framework
# (KrylovWorkspace + IterativeResult + stopping) + CG + PCG. Header-only
# templates; the .cpp carries the hesap.iterative.* CLI registration + anchor.
# Determinism moat: every inner product / norm routes through the bit-exact
# KBN-pairwise blas1 reductions (crd-hesap-dense). ADR-0065 §v4 lock at v4z.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-iterative STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-iterative
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-iterative
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math          # Complex<T>
        crd-hesap         # LinearOp<T> + CLI substrate
        crd-hesap-dense   # Vector<T> + blas1 (KBN-pairwise reductions)
        crd-hesap-sparse  # SparseLinearOp<T> for the CLI
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-iterative PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-iterative)
