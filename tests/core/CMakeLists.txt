crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-core-tests ${TEST_SOURCES})

target_link_libraries(crd-core-tests PRIVATE
    crd-core
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-core-tests)

catch_discover_tests(crd-core-tests)
