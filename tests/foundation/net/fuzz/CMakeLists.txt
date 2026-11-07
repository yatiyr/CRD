# The negotiation fuzz target (NET-STD). Its committed corpus is replayed on every lane and linux-clang-fuzz builds
# the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-net-negotiation   decode_negotiation, select_negotiation, write_answer, verify_answer and write_offer
#                              against a restated wire layout and the contract's selection and downgrade rules.
crd_fuzz_target(crd-fuzz-net-negotiation
    SOURCES fuzz_negotiation.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")

# Frames and admission (NET.3).
#   crd-fuzz-net-admission   any bytes on a stream and in datagrams, against a restated frame layout and admission rule; a
#                            stream at every split reading as it reads whole; fragments of generated messages on a hostile
#                            path; every budget given back.
crd_fuzz_target(crd-fuzz-net-admission
    SOURCES fuzz_admission.cpp LIBRARIES crd-net CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-admission")

# The generated wire formats (NET.3): scripts/gen-wire-schemas.py writes one target per schema.
#   crd-fuzz-wire-session-control   every message of the session control channel's generated decoder.
#   crd-fuzz-wire-test              every field type, shape and revision rule of the schema language.
crd_fuzz_target(crd-fuzz-wire-session-control
    SOURCES fuzz_wire_session_control.cpp LIBRARIES crd-net
    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-wire-session-control")
crd_fuzz_target(crd-fuzz-wire-test
    SOURCES fuzz_wire_test.cpp LIBRARIES crd-net-quantized CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-wire-test")
target_include_directories(crd-fuzz-wire-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(TARGET crd-fuzz-wire-test-libfuzzer)
    target_include_directories(crd-fuzz-wire-test-libfuzzer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
