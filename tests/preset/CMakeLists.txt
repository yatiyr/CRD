add_executable(crd-preset-tests
    test_preset_registry.cpp
    test_quality_preset.cpp
    test_camera_preset.cpp
    test_preset_resolver.cpp
)

target_link_libraries(crd-preset-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-resources
    crd-preset
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-preset-tests)

catch_discover_tests(crd-preset-tests)
