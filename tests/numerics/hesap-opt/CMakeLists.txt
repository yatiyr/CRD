add_executable(crd-hesap-opt-tests
    test_gradient_descent.cpp
    test_derivatives.cpp
    test_line_search.cpp
    test_conjugate_gradient.cpp
    test_momentum.cpp
    test_lbfgs.cpp
    test_lbfgsb.cpp
    test_lm.cpp
    test_lm_sparse.cpp
    test_newton.cpp
    test_newton_sparse.cpp
    test_trust_region.cpp
    test_constrained.cpp
    test_stochastic.cpp
    test_qp.cpp
    test_nlp.cpp
    test_lp.cpp
    test_conic.cpp
    test_model.cpp
    test_direct_search.cpp
    test_cobyla.cpp
    test_newuoa.cpp
    test_bobyqa.cpp
    test_global.cpp
    test_mip.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-opt-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-ordering   # SupernodalCholesky symbolic (sparse-LM normal-equations factor, v7-e-2)
    crd-hesap-direct     # SupernodalCholesky (the JᵀJ crush vehicle)
    crd-hesap-stats      # Philox counter-RNG (v7-i minibatch sampler)
    crd-hesap-opt
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-opt-tests)

crd_discover_tests(crd-hesap-opt-tests)

# The model's out-of-memory fatal, one child process per mode (a fatal cannot run in-process): an allocator that
# refuses the objective's or a constraint's function leaves the model out of memory, fatal in every build, never a
# null function stored. Source and protocol: tests/support/diag/specimens/hesap_opt_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-opt-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_opt_fatal_specimen.cpp")
target_link_libraries(crd-hesap-opt-fatal-specimen PRIVATE crd-core crd-memory crd-hesap-opt)
crd_diag_fatal_tests(crd-hesap-opt-fatal-specimen PREFIX crd-hesap-opt-fatal OWNER crd-hesap-opt-tests
    MODES model-objective-refused model-eq-refused model-ineq-refused)
