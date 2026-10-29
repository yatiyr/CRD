crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-platform-tests ${TEST_SOURCES})

target_link_libraries(crd-platform-tests PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-platform-tests)

crd_discover_tests(crd-platform-tests)
