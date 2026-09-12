add_executable(crd-chir-tests
    test_source_model.cpp  # CEIR-32b/32c: the source model + the graph-schema + the CHIR text projections
    test_chir_lower.cpp    # CEIR-32d: the CHIR->CEIR lowering + the text/graph parity gate
)

target_link_libraries(crd-chir-tests PRIVATE
    crd-core
    crd-chir
    crd-ceir  # CEIR-32d: the lowering target — print/parse/find_structure_error over the lowered module
    Catch2::Catch2WithMain
)

# CRD_REPO_DIR: the reading gate slurps the committed assets/chir/*.chirgraph (the cwd-luck-free absolute path).
target_compile_definitions(crd-chir-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-chir-tests)

catch_discover_tests(crd-chir-tests)
