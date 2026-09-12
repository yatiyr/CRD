add_executable(crd-scene-cooker-tests
    test_scene_cooker.cpp
    test_obek_cooker.cpp
)

target_link_libraries(crd-scene-cooker-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-scene
    crd-cooker
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-cooker-tests)

catch_discover_tests(crd-scene-cooker-tests)
