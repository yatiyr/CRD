add_executable(crd-cooker-tests
    test_mesh_cook_options.cpp
)

target_link_libraries(crd-cooker-tests PRIVATE
    crd-core
    crd-containers
    crd-cooker
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-cooker-tests)

catch_discover_tests(crd-cooker-tests)
