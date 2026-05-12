crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-jobs-tests ${TEST_SOURCES})

target_link_libraries(crd-jobs-tests PRIVATE
    crd-core
    crd-containers   # white-box tests include engine/jobs/src/scheduler.hpp, which uses crd::containers::ConcurrentQueue
    crd-jobs
    Catch2::Catch2
)

crd_enable_default_pch(crd-jobs-tests)

catch_discover_tests(crd-jobs-tests)
