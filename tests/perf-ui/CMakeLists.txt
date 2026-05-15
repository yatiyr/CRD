add_executable(crd-perf-ui-tests
    test_panel_helpers.cpp
    test_profiler_sources.cpp
)

target_link_libraries(crd-perf-ui-tests PRIVATE
    crd-core
    crd-memory
    crd-perf
    crd-perf-ui
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-perf-ui-tests)

catch_discover_tests(crd-perf-ui-tests)
