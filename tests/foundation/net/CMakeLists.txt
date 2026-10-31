# crd-net tests (NET-STD): the negotiation codec and the identifiers generated from the standards register.
add_executable(crd-net-tests
    test_negotiation.cpp
    test_register_ids.cpp
)

target_link_libraries(crd-net-tests PRIVATE
    crd-core
    crd-containers
    crd-net
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-net-tests)
crd_discover_tests(crd-net-tests)

# The register identifiers header is scripts/gen-register-ids.py's output for cmake/standards-register.json.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-net-register-ids-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-register-ids.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-net-register-ids-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# The negotiation decoder and handshake under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)
