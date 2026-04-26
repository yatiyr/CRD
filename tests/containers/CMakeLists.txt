crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-containers-tests ${TEST_SOURCES})

target_link_libraries(crd-containers-tests PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-containers-tests)

catch_discover_tests(crd-containers-tests)
