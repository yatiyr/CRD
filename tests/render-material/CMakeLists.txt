add_executable(crd-render-material-tests
    test_material_contract.cpp  # RAF-5 Gate 5
)

target_link_libraries(crd-render-material-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-render-asset-core
    crd-render-program
    crd-render-material
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-render-material-tests)

catch_discover_tests(crd-render-material-tests)
