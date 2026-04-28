crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-imgui-tests ${TEST_SOURCES})

target_link_libraries(crd-imgui-tests PRIVATE
    crd-core
    crd-config
    crd-imgui
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-imgui-tests)

catch_discover_tests(crd-imgui-tests)
