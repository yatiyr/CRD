add_executable(crd-hesap-eigen-tests
    test_arnoldi.cpp
    test_cli.cpp
    test_feast.cpp
    test_jacobi_davidson.cpp
    test_krylov_schur.cpp
    test_lanczos.cpp
    test_lobpcg.cpp
    test_lobpcg_anisotropic.cpp
    test_lobpcg_gen.cpp
    test_lobpcg_gen_precond.cpp
    test_lobpcg_precond.cpp
    test_shift_invert.cpp
    test_svds.cpp
)

target_link_libraries(crd-hesap-eigen-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-eigen
    crd-hesap-preconditioners
    crd-hesap-amg
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-eigen-tests)

catch_discover_tests(crd-hesap-eigen-tests)
