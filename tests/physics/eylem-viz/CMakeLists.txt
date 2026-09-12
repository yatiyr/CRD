add_executable(crd-eylem-viz-tests
    test_visualizers.cpp
)

target_link_libraries(crd-eylem-viz-tests PRIVATE
    crd-core
    crd-memory
    crd-math
    crd-scene
    crd-draw
    crd-eylem
    crd-eylem-rigid3d
    crd-eylem-viz
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-eylem-viz-tests)

catch_discover_tests(crd-eylem-viz-tests)
