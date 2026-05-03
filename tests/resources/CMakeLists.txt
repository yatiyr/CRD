crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-resources-tests ${TEST_SOURCES})

target_link_libraries(crd-resources-tests PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-log
    crd-platform
    crd-resources
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-resources-tests)

catch_discover_tests(crd-resources-tests)
