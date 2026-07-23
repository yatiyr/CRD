add_executable(crd-cooker-tests
    test_mesh_cook_options.cpp
    test_mesh_wave1.cpp
    test_texture_cook.cpp
    test_scene_decompose.cpp
    test_gltf_roundtrip.cpp # GEO-4 pt 3: import -> cook -> native edit -> export -> re-import
)

target_link_libraries(crd-cooker-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-resources
    crd-cooker
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-cooker-tests)

catch_discover_tests(crd-cooker-tests)
