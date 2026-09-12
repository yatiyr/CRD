add_executable(crd-hesap-resources-tests
    test_matrix_resource.cpp
)

target_link_libraries(crd-hesap-resources-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-hesap
    crd-hesap-sparse
    crd-hesap-resources
    crd-resources
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-resources-tests)

catch_discover_tests(crd-hesap-resources-tests)
