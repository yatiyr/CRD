add_executable(crd-hesap-opt-tests
    test_gradient_descent.cpp
    test_derivatives.cpp
    test_line_search.cpp
    test_conjugate_gradient.cpp
    test_lbfgs.cpp
    test_lbfgsb.cpp
    test_lm.cpp
    test_lm_sparse.cpp
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
    crd-hesap-opt
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-opt-tests)

catch_discover_tests(crd-hesap-opt-tests)
