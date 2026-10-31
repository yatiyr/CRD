# The negotiation fuzz target (NET-STD). Its committed corpus is replayed on every lane and linux-clang-fuzz builds
# the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-net-negotiation   decode_negotiation, select_negotiation, write_answer, verify_answer and write_offer
#                              against a restated wire layout and the contract's selection and downgrade rules.
crd_fuzz_target(crd-fuzz-net-negotiation
    SOURCES fuzz_negotiation.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
