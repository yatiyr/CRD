crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-rhi-tests ${TEST_SOURCES})

target_link_libraries(crd-rhi-tests PRIVATE
    crd-core
    crd-rhi
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-rhi-tests)

catch_discover_tests(crd-rhi-tests)
