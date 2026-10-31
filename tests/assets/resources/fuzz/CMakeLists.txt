# The cooked-pack reload fuzz target (DIAG.10a). Its committed corpus is replayed on every lane and linux-clang-fuzz
# builds the libFuzzer executable from the same source. Contract: docs/design/test-instruments.md; harness:
# tests/support/fuzz.
#   crd-fuzz-resources-reload   crdr_read and the ResourceManager's pack ingress (mount_manifest, unmount, load_sync,
#                               reload_mount_now, eviction) against a restated CRDR layout and a model of the manager.
crd_fuzz_target(crd-fuzz-resources-reload
    SOURCES fuzz_resource_reload.cpp LIBRARIES crd-resources crd-compression crd-log crd-platform
    CORPUS "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
