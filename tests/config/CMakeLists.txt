crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-config-tests ${TEST_SOURCES})

target_link_libraries(crd-config-tests PRIVATE
    crd-core
    crd-config
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-config-tests)

catch_discover_tests(crd-config-tests)
