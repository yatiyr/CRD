# crd-light-cook tests — REN-38-E1..E6 (D-007 row 141): the light array, every light type, IBL, decals,
# clustering and the shadow vocabulary beyond CSM.
add_executable(crd-light-cook-tests
    test_lighting_asset.cpp
)

target_link_libraries(crd-light-cook-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-light-cook
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-light-cook-tests)

catch_discover_tests(crd-light-cook-tests)
