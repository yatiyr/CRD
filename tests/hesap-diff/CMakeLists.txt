add_executable(crd-hesap-diff-tests
    test_diff.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-diff-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-hesap
    crd-hesap-diff
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-diff-tests)

catch_discover_tests(crd-hesap-diff-tests)
