crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-containers-tests ${TEST_SOURCES})

target_link_libraries(crd-containers-tests PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

catch_discover_tests(crd-containers-tests)
