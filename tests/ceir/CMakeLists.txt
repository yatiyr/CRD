add_executable(crd-ceir-tests
    test_ir_core.cpp  # CEIR-1a: core IR — arena storage, intrusive def-use, RAUW, O(1) op edit/erase, no-per-op-malloc
)

target_link_libraries(crd-ceir-tests PRIVATE
    crd-core
    crd-memory
    crd-ceir
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-tests)

catch_discover_tests(crd-ceir-tests)
