crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(crd-core-tests ${TEST_SOURCES})

target_link_libraries(crd-core-tests PRIVATE
    crd-core
    crd-diag-harness # DIAG.5a: runs the crash-capture specimen as a bounded child and classifies its exit + dumps
    Catch2::Catch2WithMain
)

# DIAG.5a crash-capture acceptance: the specimen is built and its path handed to the consuming test, which asserts
# the fatal path terminates with the right code and produces the right on-disk dump outcome per mode.
add_dependencies(crd-core-tests crd-diag-crash-capture-specimen)
target_compile_definitions(crd-core-tests PRIVATE
    CRD_DIAG_CRASH_CAPTURE_SPECIMEN="$<TARGET_FILE:crd-diag-crash-capture-specimen>")

crd_enable_default_pch(crd-core-tests)

crd_discover_tests(crd-core-tests)
