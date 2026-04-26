add_executable(crd-math-tests
    test_math.cpp
)

target_link_libraries(crd-math-tests PRIVATE
    crd-core
    crd-math
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-math-tests)

catch_discover_tests(crd-math-tests)
