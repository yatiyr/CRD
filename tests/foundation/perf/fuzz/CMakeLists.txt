# The crd-perf evidence-reader fuzz targets. Each committed corpus (corpus/<reader>) is replayed on every lane and
# linux-clang-fuzz builds the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md;
# harness: tests/support/fuzz.
#   crd-fuzz-perf-bundle   import_bundle, the bounded non-executing bundle importer (REPO.DEV.9; DIAG.5d(e2)): the
#                          fuzzed proof of "reject traversal / oversized / decompression-bomb inputs".
#   crd-fuzz-perf-flight   decode_flight_record, the flight-record reader (DIAG.10a), against a model of what a
#                          FlightRecorder can write and the canonical encoding.
#   crd-fuzz-perf-assets   the flight-policy and fault-plan asset readers, the fault-plan text and crd-toml beneath
#                          them (DIAG.10a): a refusal leaves the output untouched, an accepted document round-trips.
#   crd-fuzz-perf-capture  validate_capture_buffer and CaptureView, the CPROF capture reader (DIAG.10a): every view
#                          accessor stays inside the buffer and every name it hands out is terminated there.
crd_fuzz_target(crd-fuzz-perf-bundle
    SOURCES fuzz_bundle_import.cpp LIBRARIES crd-perf CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/bundle")
crd_fuzz_target(crd-fuzz-perf-flight
    SOURCES fuzz_flight_record.cpp LIBRARIES crd-perf CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/flight")
crd_fuzz_target(crd-fuzz-perf-assets
    SOURCES fuzz_diag_assets.cpp LIBRARIES crd-perf crd-toml CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/assets")
crd_fuzz_target(crd-fuzz-perf-capture
    SOURCES fuzz_capture_view.cpp LIBRARIES crd-perf CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus/capture")
