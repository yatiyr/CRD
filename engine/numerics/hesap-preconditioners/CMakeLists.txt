# crd-hesap-preconditioners -- Phase 3.1.6 v4a.
#
# Preconditioners as LinearOp<T> exposing M⁻¹. v4a: Jacobi (v4a-1); block-Jacobi
# + SSOR (v4a-2). IC/ILU/SPAI/Schwarz land v4g+. Depends on crd-hesap-iterative
# ONE-WAY (its CLI demonstrates pcg = solver+preconditioner via the header-only
# cg.hpp); iterative never depends on preconditioners (a preconditioner is just
# a LinearOp). No cycle.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-preconditioners STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-preconditioners
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-preconditioners
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap            # LinearOp<T> + CLI substrate
        crd-hesap-dense      # Vector<T> + blas1 + small dense LU (block-Jacobi, v4a-2)
        crd-hesap-sparse     # SparseMatrix CSR
        crd-hesap-ordering   # amd_order (AMD fill-reducing reorder; reordered.hpp, matches Eigen ILUT)
        crd-hesap-iterative  # cg.hpp (header-only) for the pcg CLI demo
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-preconditioners PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-preconditioners)
