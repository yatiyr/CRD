add_executable(crd-time-tests
    test_instant_duration.cpp
    test_clocks.cpp
    test_stopwatch.cpp
    test_frame_clock.cpp
    test_deterministic_clock.cpp
    test_deadline.cpp
    test_gpu_timestamp.cpp
    test_rational_time.cpp
)

target_link_libraries(crd-time-tests PRIVATE
    crd-core
    crd-units
    crd-time
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-time-tests)

catch_discover_tests(crd-time-tests)
