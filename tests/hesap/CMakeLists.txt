add_executable(crd-hesap-tests
    test_complex.cpp
    test_linear_op.cpp
    test_handles.cpp
    test_crdr_format.cpp
    test_json_writer.cpp
    test_command_schema.cpp
    test_command_registry.cpp
    test_mcp_descriptor.cpp
    test_arg_value.cpp
)

target_link_libraries(crd-hesap-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tests)

catch_discover_tests(crd-hesap-tests)
