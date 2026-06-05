add_executable(crd-hesap-direct-tests
    test_blr.cpp
    test_cli.cpp
    test_frontal.cpp
    test_gmres_refine.cpp
    test_hss.cpp
    test_hss_ulv.cpp
    test_lu_partial_pivot.cpp
    test_mixed_qr_refine.cpp
    test_mixed_refine.cpp
    test_multifrontal_cholesky_blr.cpp
    test_multifrontal_lu_pp.cpp
    test_multifrontal_ldlt.cpp
    test_multifrontal_qr.cpp
    test_sparse_lu.cpp
    test_supernodal.cpp
    test_supernodal_lu.cpp
)

target_link_libraries(crd-hesap-direct-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-sparse
    crd-hesap-ordering
    crd-hesap-direct
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-direct-tests)

catch_discover_tests(crd-hesap-direct-tests)
