add_executable(crd-draw-tests
    test_v1a_d0_buffer_and_shapes.cpp
    test_debug_viz.cpp
    test_ckir_draw_shaders.cpp # REN-38-F7: the AUTHORED draw suite gate (cook + contract + GLSL/HLSL lowering)
)

target_link_libraries(crd-draw-tests PRIVATE
    crd-core
    crd-containers
    crd-math
    crd-scene
    crd-draw
    crd-vertex-cook
    crd-material-cook
    crd-platform
    Catch2::Catch2WithMain
)

# the drift gate's std::getenv (the tests/anim + scene-render precedent)
target_compile_definitions(crd-draw-tests PRIVATE _CRT_SECURE_NO_WARNINGS)

crd_enable_default_pch(crd-draw-tests)

# REN-38-F7: the drift gate compares the embedded pack against the shipped assets
catch_discover_tests(crd-draw-tests
    PROPERTIES ENVIRONMENT "CRD_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets")
