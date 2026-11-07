# crd-compression tests (OWN.1, NET.3): the engine's raw DEFLATE, zlib and Adler-32, the zstd binding, the entropy coder
# and the ZIP container.
add_executable(crd-compression-tests
    test_deflate.cpp
    test_entropy.cpp
    test_zip.cpp
    test_zstd.cpp
    test_zstd_messages.cpp
)

target_include_directories(crd-compression-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(crd-compression-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-compression
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-compression-tests)
crd_discover_tests(crd-compression-tests)

# The entropy coder's known answers are scripts/gen-entropy-vectors.py's, a restatement that shares no code with it.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-compression-entropy-vectors-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-entropy-vectors.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-compression-entropy-vectors-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# Coded messages against any bytes.
add_subdirectory(fuzz)
