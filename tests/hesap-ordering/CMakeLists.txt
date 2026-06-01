add_executable(crd-hesap-ordering-tests
    test_ordering.cpp
    test_mc64.cpp
    test_lu_symbolic.cpp
)

target_link_libraries(crd-hesap-ordering-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-sparse
    crd-hesap-ordering
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-ordering-tests)

catch_discover_tests(crd-hesap-ordering-tests)
