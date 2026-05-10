add_executable(crd-eylem-tests
    test_v1a_interface.cpp
)

target_link_libraries(crd-eylem-tests PRIVATE
    crd-core
    crd-containers
    crd-math
    crd-eylem
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-eylem-tests)

catch_discover_tests(crd-eylem-tests)
