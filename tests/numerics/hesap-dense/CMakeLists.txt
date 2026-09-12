add_executable(crd-hesap-dense-tests
    test_matrix_catalog.cpp
    test_vector.cpp
    test_blas1_real.cpp
    test_blas1_complex.cpp
    test_blas1_cli.cpp
    test_matrix.cpp
    test_blas2_real.cpp
    test_blas2_complex.cpp
    test_blas2_banded.cpp
    test_blas3_real.cpp
    test_blas3_complex.cpp
    test_blas3_parallel.cpp
    test_parallel_triangular.cpp
    test_lu.cpp
    test_cholesky.cpp
    test_ldlt.cpp
    test_qr.cpp
    test_cod.cpp
    test_interp_decomp.cpp
    test_randomized_range.cpp
    test_lstsq.cpp
    test_nnls_tls.cpp
    test_eig_nonsym.cpp
    test_linear_op_condition.cpp
    test_refinement.cpp
    test_solvers_cli.cpp
    test_random_property.cpp
    test_eig_sym.cpp
    test_eig_dc.cpp
    test_mrrr_eigenvalues.cpp
    test_svd.cpp
)

target_link_libraries(crd-hesap-dense-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-dense
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-dense-tests)

catch_discover_tests(crd-hesap-dense-tests)
