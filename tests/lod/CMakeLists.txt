add_executable(crd-lod-tests
    test_lod_chain.cpp # REN-40-C1: the mesh LOD chain
)

target_link_libraries(crd-lod-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-resources
    crd-lod
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-lod-tests)

catch_discover_tests(crd-lod-tests)
