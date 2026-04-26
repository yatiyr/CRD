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

# Benchmarks are intentionally built with the regular test tree but kept out of
# ctest so CI/unit-test presets stay fast and deterministic.
