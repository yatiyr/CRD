add_executable(crd-hesap-direct-tests
    test_cli.cpp
    test_frontal.cpp
    test_supernodal.cpp
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
