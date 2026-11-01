# crd-comply tests (COMPLY.0 and COMPLY.1): rule generation decoding, the live store and the query, the committed
# generations, the name matcher, sanctions screening and the official lists as imported.
add_executable(crd-comply-tests
    test_imported.cpp
    test_names.cpp
    test_rules.cpp
    test_sanctions.cpp
)

target_link_libraries(crd-comply-tests PRIVATE
    crd-comply
    crd-containers
    crd-core
    crd-platform
    Catch2::Catch2WithMain
)

# The committed generations are decoded from the source tree.
target_compile_definitions(crd-comply-tests PRIVATE CRD_COMPLY_RULES_DIR="${CMAKE_SOURCE_DIR}/assets/comply"
                           CRD_COMPLY_IMPORTED_DIR="${CMAKE_CURRENT_BINARY_DIR}/sanctions-lists")

crd_enable_default_pch(crd-comply-tests)
crd_discover_tests(crd-comply-tests)

# assets/comply is scripts/gen-comply-rules.py's output for cmake/legal-register.json.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-comply-rules-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-comply-rules.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-comply-rules-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
    # assets/comply/crypto-inventory.json is scripts/gen-crypto-inventory.py's output for the module registry and
    # cmake/standards-register.json (COMPLY.1); crd-check-registers holds each export to its reviewed classification.
    add_test(NAME crd-comply-crypto-inventory-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-crypto-inventory.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-comply-crypto-inventory-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
    # The official sanctions lists end to end (COMPLY.1): scripts/import-sanctions.py imports the real-byte excerpts
    # in lists/ (entries cut whole from the files read on 2026-11-01; OFAC's Record_Count set to the entries kept)
    # into an emptied directory, with the US, EU and UK embargoed destinations authored in
    # cmake/legal-register.json, whose first generations the hidden [.imported] cases decode, adopt and screen.
    set(_crd_imported "${CMAKE_CURRENT_BINARY_DIR}/sanctions-lists")
    add_test(NAME crd-comply-sanctions-clean COMMAND ${CMAKE_COMMAND} -E rm -rf "${_crd_imported}")
    set_tests_properties(crd-comply-sanctions-clean PROPERTIES FIXTURES_SETUP crd-comply-sanctions-empty)
    add_test(NAME crd-comply-sanctions-import
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/import-sanctions.py" --root "${CMAKE_SOURCE_DIR}"
                     import --from "${CMAKE_CURRENT_SOURCE_DIR}/lists" --out "${_crd_imported}"
                     --authority us.ofac.sdn --authority eu.fsf --authority un.sc --authority gb.fcdo
                     --authority us.ofac.embargo --authority eu.council.embargo --authority gb.fcdo.embargo)
    set_tests_properties(crd-comply-sanctions-import PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1"
                         FIXTURES_REQUIRED crd-comply-sanctions-empty FIXTURES_SETUP crd-comply-sanctions-lists)
    add_test(NAME crd-comply-sanctions-imported COMMAND crd-comply-tests "[.imported]")
    set_tests_properties(crd-comply-sanctions-imported PROPERTIES TIMEOUT 120
                         FIXTURES_REQUIRED crd-comply-sanctions-lists)
endif()

# The rule generation decoder and store under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)
