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
    test_log_density_grad.cpp
    test_descriptive.cpp
    test_hypothesis.cpp
    test_resampling.cpp
    test_kde.cpp
    test_robust.cpp
    test_streaming.cpp
    test_mcmc.cpp
    test_regression.cpp
    test_cli.cpp
    test_stats_kernels.cpp
)

target_link_libraries(crd-hesap-stats-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-stats
    crd-hesap-special
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-stats-tests)

crd_discover_tests(crd-hesap-stats-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-hesap-stats-kernels-at-<level>).
crd_add_kernel_level_tests(crd-hesap-stats-tests "[kernels]")
