add_executable(crd-draw-tests
    test_v1a_d0_buffer_and_shapes.cpp
    test_debug_viz.cpp
)

target_link_libraries(crd-draw-tests PRIVATE
    crd-core
    crd-containers
    crd-math
    crd-scene
    crd-draw
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-draw-tests)

catch_discover_tests(crd-draw-tests)
