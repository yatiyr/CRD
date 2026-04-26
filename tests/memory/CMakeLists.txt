crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-memory-tests ${TEST_SOURCES})

target_link_libraries(crd-memory-tests PRIVATE
    crd-core
    crd-log
    crd-memory
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-memory-tests)

catch_discover_tests(crd-memory-tests)
