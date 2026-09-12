add_executable(crd-render-pass-tests
    test_executor_registry.cpp  # RAF-6 Gate 6
)

target_link_libraries(crd-render-pass-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-render-asset-core
    crd-render-pass
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-render-pass-tests)

catch_discover_tests(crd-render-pass-tests)
