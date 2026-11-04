add_executable(crd-json-tests test_json.cpp)

target_link_libraries(crd-json-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-json
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-json-tests)
crd_discover_tests(crd-json-tests)
