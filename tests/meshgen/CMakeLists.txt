add_executable(crd-meshgen-tests
    test_meshgen.cpp
)

target_link_libraries(crd-meshgen-tests PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-resources
    crd-meshgen
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-meshgen-tests)

catch_discover_tests(crd-meshgen-tests)
