# The run-record fuzz target (DIAG.10a). Its committed corpus is replayed on every lane and linux-clang-fuzz builds
# the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness: tests/support/fuzz.
#   crd-fuzz-ceir-record   decode_record, the run-record reader replay.run, replay.prepare and the host and device
#                          replays start from, against what the three recorders can write, the canonical encoding and
#                          an allocation bound proportional to the input.
crd_fuzz_target(crd-fuzz-ceir-record
    SOURCES fuzz_replay_record.cpp LIBRARIES crd-ceir-cook CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
