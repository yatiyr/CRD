# crd-comply tests (COMPLY.0): rule generation decoding, the live store and the query, and the committed generations.
add_executable(crd-comply-tests
    test_rules.cpp
)

target_link_libraries(crd-comply-tests PRIVATE
    crd-comply
    crd-containers
    crd-core
    crd-platform
    Catch2::Catch2WithMain
)

# The committed generations are decoded from the source tree.
target_compile_definitions(crd-comply-tests PRIVATE CRD_COMPLY_RULES_DIR="${CMAKE_SOURCE_DIR}/assets/comply")

crd_enable_default_pch(crd-comply-tests)
crd_discover_tests(crd-comply-tests)

# assets/comply is scripts/gen-comply-rules.py's output for cmake/legal-register.json.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-comply-rules-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-comply-rules.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-comply-rules-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()

# The rule generation decoder and store under the bounded fuzz harness, with its committed corpus.
add_subdirectory(fuzz)
