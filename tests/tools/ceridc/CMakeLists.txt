add_executable(crd-ceridc-tests
    test_ceridc.cpp # GEO-11: THE AGENT GATE — the full scenario through MCP alone + the stdio transport smoke
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
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-ceridc-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # std::getenv/system (the stdio smoke)
endif()

crd_enable_default_pch(crd-ceridc-tests)

catch_discover_tests(crd-ceridc-tests
    PROPERTIES ENVIRONMENT "CRD_CERIDC_EXE=$<TARGET_FILE:ceridc>"
)
