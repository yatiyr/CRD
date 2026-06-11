add_executable(crd-hesap-stats-tests
    test_philox.cpp
    test_normal.cpp
)

target_link_libraries(crd-hesap-stats-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-stats
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-stats-tests)

catch_discover_tests(crd-hesap-stats-tests)
