add_executable(crd-ceridc-tests
    test_ceridc.cpp # GEO-11: THE AGENT GATE — the full scenario through MCP alone + the stdio transport smoke
    test_ceridc_inspect.cpp # DIAG.8b: the headless inspection consumer — `ceridc inspect` on the committed authored program
    test_ceridc_diag.cpp # the diag verb: native, CLI verb, MCP tool and the real binary return the same bounded result
    test_ceridc_comply.cpp # COMPLY.0: the comply verb and the real binary answer the committed rule generations
    test_ceridc_screen.cpp # COMPLY.1: the screen verb and the real binary screen against sanctions lists
    test_ceridc_policy.cpp # NET.2: the policy verb renders, joins and judges the crypto policy generation
    test_ceridc_model.cpp # NET.3: the model verb trains and measures a channel's body models
)

target_link_libraries(crd-ceridc-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-asset-io
    crd-json
    crd-resources
    crd-ceridc
    crd-cooker
    crd-perf
    crd-jobs # DIAG.9a: the host provider's pool for the native and in-process replay calls (a Catch listener)
    crd-ceir # DIAG.9a: SeededInputs::draw, to choose a failing seed independently of the commands
    crd-ceir-cook # DIAG.9a: decode_record, to compare an inspected record with replay.record's
    crd-kir # DIAG.9a: the device record fixture builds its kernels and writes them as CKIR text
    crd-crypto # NET.2: the policy cases sign with provider keys and decode what the verb writes
    crd-net # NET.3: the model cases load what the verb writes and code the samples themselves
    Catch2::Catch2WithMain
)

# DIAG.8b: the inspect gate runs the committed authored program assets/ceir/inspect_demo.ceir.
target_compile_definitions(crd-ceridc-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")
# NET.3: the model cases train on the coded-frame tests' samples and compare with the models their schema pins.
target_include_directories(crd-ceridc-tests PRIVATE "${CMAKE_SOURCE_DIR}/tests/foundation/net")

crd_enable_default_pch(crd-ceridc-tests)

# Every case and the ceridc processes it starts write their files in the working directory, so each case gets its own
# (CTest runs the cases in parallel).
crd_discover_tests(crd-ceridc-tests CASE_DIRECTORIES
    PROPERTIES ENVIRONMENT "CRD_CERIDC_EXE=$<TARGET_FILE:ceridc>"
)

# MCP-ADAPT.0: the conformance oracle. The real binary's replies in both revisions against the published MCP schemas
# pinned in mcp/ (with their fetch records), and each tool's structured result against its own output schema
# (docs/design/agent-mcp.md). It writes only under its own temporary directory.
add_test(NAME crd-ceridc-mcp-conformance
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/mcp_conformance.py" --ceridc "$<TARGET_FILE:ceridc>")
set_tests_properties(crd-ceridc-mcp-conformance PROPERTIES TIMEOUT 300 ENVIRONMENT "PYTHONUTF8=1")

# DIAG.10a: the command-decoding fuzz target and its committed corpus replay.
add_subdirectory(fuzz)
