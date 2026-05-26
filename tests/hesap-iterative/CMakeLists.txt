add_executable(crd-hesap-iterative-tests
    test_cg.cpp
    test_gmres.cpp
    test_bicgstab.cpp
    test_minres.cpp
    test_symmlq.cpp
    test_lsqr.cpp
    test_qmr.cpp
    test_idrs.cpp
    test_gcr.cpp
    test_gcrot.cpp
    test_rminres.cpp
    test_krylov_preconditioner.cpp
    test_block_cg.cpp
    test_block_gmres.cpp
    test_block_bicgstab.cpp
    test_ic0.cpp
    test_ilu0.cpp
    test_ilut.cpp
    test_ilup.cpp
    test_spai.cpp
    test_reordered.cpp
    test_chebyshev.cpp
    test_schwarz.cpp
    test_multilevel_ilu.cpp
)

target_link_libraries(crd-hesap-iterative-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-iterative
    crd-hesap-preconditioners
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-iterative-tests)

catch_discover_tests(crd-hesap-iterative-tests)
