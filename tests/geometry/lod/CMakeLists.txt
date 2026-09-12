add_executable(crd-lod-tests
    test_lod_chain.cpp # REN-40-C1: the mesh LOD chain
    test_lod_asset.cpp # REN-40-C1: the .crdlod policy
    test_lod_atlas.cpp # REN-40-C5: the impostor atlas baker
    test_lod_impostor_pipeline.cpp # REN-40-C5.6: pipeline gates
)

target_link_libraries(crd-lod-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-containers
    crd-resources
    crd-lod
    crd-scene-render # C5.6: buffer layout constants
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-lod-tests)

catch_discover_tests(crd-lod-tests)
