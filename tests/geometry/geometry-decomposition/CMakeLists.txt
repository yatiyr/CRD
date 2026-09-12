crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-geometry-decomposition-tests ${TEST_SOURCES})

target_link_libraries(crd-geometry-decomposition-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-geometry-primitives
    crd-geometry-mesh
    crd-geometry-decomposition
    crd-jobs
    crd-perf
    crd-test-helpers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-decomposition-tests)

catch_discover_tests(crd-geometry-decomposition-tests)
