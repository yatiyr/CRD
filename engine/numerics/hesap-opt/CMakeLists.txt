# crd-hesap-opt — Phase 3.1.6 v7. OPTIMISATION (the full domain: unconstrained + constrained), matrix-free over
# the Objective<T> interface. ADR-0090. The universal "find the best X" substrate (eylem / control / FEA-CFD /
# CAD / ML / rendering / games / robotics / DAW consumers — see ADR-0090 + the phase doc).
#
# Edges (acyclic; opt is high in the hesap stack):
#   crd-hesap        — LinearOp<T> (objectives over a sparse operator) + the cli substrate
#   crd-hesap-dense  — dense Vector + blas1 (the optimizer's serial scalar arithmetic) + small dense solves
#   crd-hesap-sparse — SparseLinearOp / ParallelSpmvLeastSquaresOp (parallel-but-bit-exact objective eval)
#   crd-jobs         — the parallel objective eval is the only parallel step; the moat keeps it bit-exact
#   (LATER, named here so methods don't re-litigate): crd-hesap-direct/iterative (Newton + KKT solves; v7-g/n),
#    crd-hesap-eig (exact trust-region / negative-curvature; v7-h). Linked when those subslices land.
#
# Determinism moat (ADR-0090): the optimizer is SERIAL scalar arithmetic; the objective eval may be parallel but
# bit-exact ⇒ the optimization TRAJECTORY + result are bit-identical across {1..16} workers (the differentiator
# no mainstream optimizer carries). Reverse-mode autodiff plugs into the Objective gradient interface when it ships.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-opt STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-opt
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-opt
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap-autodiff # Dual<T> forward-AD (migrated here in v15-a; opt re-exports via opt/dual.hpp) — ADR-0097
        crd-hesap          # LinearOp<T> + the cli substrate
        crd-hesap-dense    # dense Vector + blas1 (the optimizer arithmetic)
        crd-hesap-sparse   # SparseLinearOp / ParallelSpmvLeastSquaresOp (parallel-bit-exact objective eval)
        crd-hesap-stats    # Philox counter-RNG (v7-i reproducible minibatch sampling; the v12-pull)
        crd-jobs           # parallel objective eval (the only parallel step; the moat keeps it bit-exact)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-opt PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-opt)
