crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-log-tests ${TEST_SOURCES})

target_link_libraries(crd-log-tests PRIVATE
    crd-core
    crd-log
    Catch2::Catch2WithMain
)

catch_discover_tests(crd-log-tests)
