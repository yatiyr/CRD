# crd-net tests (NET-STD, NET.3): the negotiation codec, the identifiers generated from the standards register and the
# generated wire formats, whose test schema holds quantised fields (crd-net-quantized) and fills the S18 artefact
# (crd-crypto).
add_executable(crd-net-tests
    test_negotiation.cpp
    test_register_ids.cpp
    test_wire.cpp
)

target_include_directories(crd-net-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(crd-net-tests PRIVATE
    crd-core
    crd-containers
    crd-crypto
    crd-net
    crd-net-quantized
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
    # The wire formats are scripts/gen-wire-schemas.py's output for their authored schemas, and the generator's own
    # refusals (recursion, a changed revision, a bound that does not fit, ...) hold it.
    add_test(NAME crd-net-wire-schemas-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-wire-schemas.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-net-wire-schemas-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
    add_test(NAME crd-net-wire-schemas-tool
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-wire-schemas.py")
    set_tests_properties(crd-net-wire-schemas-tool PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# The negotiation decoder and handshake under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)
