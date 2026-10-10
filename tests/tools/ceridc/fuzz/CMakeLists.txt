# The command-decoding fuzz target (DIAG.10a). Its committed corpus is replayed on every lane and linux-clang-fuzz
# builds the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-ceridc-diag   ceridc's MCP transport (mcp_handle over the owned JSON parser), its diag tool's argument
#                          decoding and the typed diagnostic command service with every command ceridc and the sandbox
#                          register, against an independent RFC 8259 validator, the JSON-RPC rules and the service's
#                          documented refusal order, paging and document contract.
crd_fuzz_target(crd-fuzz-ceridc-diag
    SOURCES fuzz_diag_commands.cpp LIBRARIES crd-ceridc crd-perf crd-asset-io CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
