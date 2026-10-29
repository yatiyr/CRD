crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-app-tests ${TEST_SOURCES})

target_link_libraries(crd-app-tests PRIVATE
    crd-core
    crd-app
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-app-tests)

crd_discover_tests(crd-app-tests)
