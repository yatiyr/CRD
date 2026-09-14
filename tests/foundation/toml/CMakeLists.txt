add_executable(crd-toml-tests test_toml.cpp)

target_link_libraries(crd-toml-tests PRIVATE
    crd-core
    crd-containers
    crd-toml
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-toml-tests)
crd_discover_tests(crd-toml-tests)
