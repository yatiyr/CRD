# crd-compression tests (OWN.1): the engine's raw DEFLATE, zlib and Adler-32, and the zstd binding.
add_executable(crd-compression-tests
    test_deflate.cpp
    test_zstd.cpp
)

target_link_libraries(crd-compression-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-compression
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-compression-tests)
crd_discover_tests(crd-compression-tests)
