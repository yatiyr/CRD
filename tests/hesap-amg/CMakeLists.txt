add_executable(crd-hesap-amg-tests
    test_amg.cpp
)

target_link_libraries(crd-hesap-amg-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-iterative   # CG/FGMRES to drive the AMG preconditioner in tests
    crd-hesap-amg
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-amg-tests)

catch_discover_tests(crd-hesap-amg-tests)
