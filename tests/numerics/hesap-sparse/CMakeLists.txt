add_executable(crd-hesap-sparse-tests
    test_substrate.cpp
    test_triplet_compress.cpp
    test_cli.cpp
    test_csc_queries.cpp
    test_spmv.cpp
    test_sell.cpp
    test_sell_parallel.cpp
    test_convert.cpp
    test_element_wise.cpp
    test_structural.cpp
    test_spgemm.cpp
    test_spgemm_parallel.cpp
    test_spmm.cpp
    test_sddmm.cpp
    test_bsr.cpp
    test_ell_dia.cpp
    test_matrix_market.cpp
    test_spgemm_hash.cpp
)

target_link_libraries(crd-hesap-sparse-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-hesap
    crd-hesap-sparse
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-sparse-tests)

catch_discover_tests(crd-hesap-sparse-tests)
