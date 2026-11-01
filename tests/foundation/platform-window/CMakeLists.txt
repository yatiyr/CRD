crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-platform-window-tests ${TEST_SOURCES})

target_link_libraries(crd-platform-window-tests PRIVATE
    crd-core
    crd-platform-window
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-platform-window-tests)

crd_discover_tests(crd-platform-window-tests)
