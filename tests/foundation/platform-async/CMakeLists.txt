add_executable(crd-platform-async-tests test_async_file.cpp)

target_link_libraries(crd-platform-async-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-platform-async
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-platform-async-tests)

crd_discover_tests(crd-platform-async-tests)
