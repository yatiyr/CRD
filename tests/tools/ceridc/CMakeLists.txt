add_executable(crd-ceridc-tests
    test_ceridc.cpp # GEO-11: THE AGENT GATE — the full scenario through MCP alone + the stdio transport smoke
    test_ceridc_inspect.cpp # DIAG.8b: the headless inspection consumer — `ceridc inspect` on the committed authored program
    test_ceridc_diag.cpp # the diag verb: native, CLI verb, MCP tool and the real binary return the same bounded result
)

target_link_libraries(crd-ceridc-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-asset-io
    crd-resources
    crd-ceridc
    crd-cooker
    crd-perf
    crd-jobs # DIAG.9a: the host provider's pool for the native and in-process replay calls (a Catch listener)
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-ceridc-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # std::getenv/system (the stdio smoke)
endif()

# DIAG.8b: the inspect gate runs the committed authored program assets/ceir/inspect_demo.ceir.
target_compile_definitions(crd-ceridc-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

crd_enable_default_pch(crd-ceridc-tests)

crd_discover_tests(crd-ceridc-tests
    PROPERTIES ENVIRONMENT "CRD_CERIDC_EXE=$<TARGET_FILE:ceridc>"
)
