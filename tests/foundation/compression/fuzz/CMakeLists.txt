# Coded messages (NET.3). Its committed corpus is replayed on every lane and linux-clang-fuzz builds the libFuzzer
# executable from the same source. Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
#   crd-fuzz-compression-messages   any bytes as an entropy coding (what decodes re-encodes to itself) and as a dictionary
#                                   frame (what decodes compresses and decodes back), and any bytes coded both ways.
crd_fuzz_target(crd-fuzz-compression-messages
    SOURCES fuzz_messages.cpp LIBRARIES crd-compression crd-memory CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus-messages")
