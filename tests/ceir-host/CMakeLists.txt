add_executable(crd-ceir-host-tests
    test_host_provider.cpp # CEIR-6b: task.parallel_for on crd::jobs -- map correctness, num_jobs determinism, pre-flight, errors
)

target_link_libraries(crd-ceir-host-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-host
    crd-jobs            # the test binary owns the jobs pool lifecycle (init/shutdown via a Catch listener)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-host-tests)
catch_discover_tests(crd-ceir-host-tests)
