# crd-sandbox-inspect-tests — DIAG.8b: the sandbox's runtime-inspection consumer, without a window.
#
# Compiles `sandbox/src/inspect_panel.cpp` directly (the showcase-tests precedent: the panel is a sandbox-internal TU)
# and drives it the way the sandbox's frame loop does. The panel loads the committed authored program through the
# renderer's app-first program seam, so the test constructs a SceneRenderer for its asset mounts only (no device).
# The engine asset root is a compiled path (a ctest ENVIRONMENT carries one variable; see the scene-render tests).

add_executable(crd-sandbox-inspect-tests
    test_inspect_panel.cpp
    "${CMAKE_SOURCE_DIR}/sandbox/src/inspect_panel.cpp"
)

target_include_directories(crd-sandbox-inspect-tests PRIVATE "${CMAKE_SOURCE_DIR}/sandbox/src")

target_link_libraries(crd-sandbox-inspect-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-render-asset-core
    crd-scene-render
    crd-ceir
    crd-ceir-cook
    Catch2::Catch2WithMain
)

target_compile_definitions(crd-sandbox-inspect-tests PRIVATE
    CRD_SANDBOX_INSPECT_ENGINE_ASSETS="${CMAKE_SOURCE_DIR}/assets")

crd_enable_default_pch(crd-sandbox-inspect-tests)
crd_discover_tests(crd-sandbox-inspect-tests)
