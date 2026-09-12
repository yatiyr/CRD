# crd-material-cook tests — REN-38-C1/C2/C3 (D-007 row 141): the MATERIAL as an authored `.crdm`, its node-set
# COVERAGE, its round-trip, and INSTANCES with parameter overrides.
add_executable(crd-material-cook-tests
    test_material_asset.cpp
)

target_link_libraries(crd-material-cook-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-material-cook
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-material-cook-tests)

catch_discover_tests(crd-material-cook-tests)
