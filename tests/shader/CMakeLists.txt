crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-shader-tests ${TEST_SOURCES})

target_link_libraries(crd-shader-tests PRIVATE
    crd-core
    crd-platform
    crd-shader
    Catch2::Catch2WithMain
)

target_compile_definitions(crd-shader-tests PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-shader-tests)

catch_discover_tests(crd-shader-tests)
