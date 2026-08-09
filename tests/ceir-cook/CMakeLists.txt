add_executable(crd-ceir-cook-tests
    test_program_cook.cpp    # CEIR-7a: cook a module -> a self-describing CRDR blob; round-trip, cross-Context reload, verify-reject, header validation
    test_runtime_program.cpp # CEIR-7b: RuntimeProgram + generation-safe RuntimeSlot handle; hash-validated load, splice-reject, generation staleness
)

target_link_libraries(crd-ceir-cook-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    crd-ceir-cook
    crd-resources      # the test FORGES CRDR containers directly (missing-CDEP / wrong-type) — the ceir-host precedent
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-cook-tests)
catch_discover_tests(crd-ceir-cook-tests)
