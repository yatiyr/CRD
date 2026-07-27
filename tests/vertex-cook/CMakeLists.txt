# crd-vertex-cook tests — REN-38-D1/D2/D3/D4 (D-007 row 141): the VERTEX PROGRAM as an authored `.crdv`,
# its skinning + morph schemes, its displacement graph, and the VS↔FS varying CONTRACT.
add_executable(crd-vertex-cook-tests
    test_vertex_asset.cpp
)

target_link_libraries(crd-vertex-cook-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-material-cook
    crd-vertex-cook
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-vertex-cook-tests)

catch_discover_tests(crd-vertex-cook-tests)
