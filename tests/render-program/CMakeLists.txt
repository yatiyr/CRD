add_executable(crd-render-program-tests
    test_program_contract.cpp  # RAF-4 Gate 4
)

target_link_libraries(crd-render-program-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-render-asset-core
    crd-render-program
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-render-program-tests)

catch_discover_tests(crd-render-program-tests)
