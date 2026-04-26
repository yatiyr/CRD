add_executable(crd-bench
    test_bench.cpp
)

target_link_libraries(crd-bench PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-bench)

catch_discover_tests(crd-bench)
