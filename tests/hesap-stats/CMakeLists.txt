add_executable(crd-hesap-stats-tests
    test_philox.cpp
    test_normal.cpp
    test_rng.cpp
    test_samplers.cpp
    test_qmc.cpp
    test_continuous.cpp
    test_discrete.cpp
    test_heavy.cpp
    test_multivariate.cpp
)

target_link_libraries(crd-hesap-stats-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-stats
    crd-hesap-special
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-stats-tests)

catch_discover_tests(crd-hesap-stats-tests)
