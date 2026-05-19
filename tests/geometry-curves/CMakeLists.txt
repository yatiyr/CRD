add_executable(crd-geometry-curves-tests
    test_curves_types.cpp
    test_curves_validate.cpp
    test_curves_evaluator.cpp
    test_curves_sample.cpp
    test_curves_arclength.cpp
    test_curves_queries.cpp
    test_curves_frames.cpp
    test_curves_typed.cpp
)

target_link_libraries(crd-geometry-curves-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-units
    crd-geometry-primitives
    crd-geometry-curves
    crd-test-helpers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-geometry-curves-tests)

catch_discover_tests(crd-geometry-curves-tests)
