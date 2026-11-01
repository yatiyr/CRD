# crd-crypto tests: the algorithm identifiers generated from the standards register and the signed crypto policy
# generations (NET-STD), and the provider interface against Project Wycheproof's vectors (NET.2).
add_executable(crd-crypto-tests
    test_policy.cpp
    test_provider.cpp
    test_register_ids.cpp
)

target_link_libraries(crd-crypto-tests PRIVATE
    crd-asset-io
    crd-core
    crd-containers
    crd-crypto
    crd-memory
    crd-platform
    Catch2::Catch2WithMain
)

# The pinned Wycheproof vectors are read from the package cache at run time (cmake/pins.json: wycheproof).
target_compile_definitions(crd-crypto-tests PRIVATE CRD_WYCHEPROOF_DIR="${wycheproof_SOURCE_DIR}")

crd_enable_default_pch(crd-crypto-tests)
crd_discover_tests(crd-crypto-tests)

# The register identifiers header is scripts/gen-register-ids.py's output for cmake/standards-register.json; the
# check compares every owner's header, so crd-net-register-ids-drift and this test run the same command.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-crypto-register-ids-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-register-ids.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-crypto-register-ids-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# The policy decoder and store under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)
