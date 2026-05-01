crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-renderer-tests ${TEST_SOURCES})

target_link_libraries(crd-renderer-tests PRIVATE
    crd-core
    crd-platform
    crd-rhi
    crd-rhi-vulkan
    crd-shader
    crd-renderer
    Catch2::Catch2WithMain
)

target_compile_definitions(crd-renderer-tests PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-renderer-tests)

catch_discover_tests(crd-renderer-tests)
