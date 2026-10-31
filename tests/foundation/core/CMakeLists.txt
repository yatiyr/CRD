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
# Its fiber modes exist where crd-jobs builds (not on ARM64 before JOBS-PORT.2's fiber context).
target_compile_definitions(crd-core-tests PRIVATE
    CRD_DIAG_CRASH_CAPTURE_SPECIMEN="$<TARGET_FILE:crd-diag-crash-capture-specimen>"
    "CRD_DIAG_FIBER_MODES=$<TARGET_EXISTS:crd-jobs>")

crd_enable_default_pch(crd-core-tests)

# DIAG.11c: the [diag-workflow] cases are the native diagnostic workflow; their CTests carry its label.
crd_discover_tests(crd-core-tests TEST_SPEC "~[diag-workflow]")
crd_discover_tests(crd-core-tests TEST_SPEC "[diag-workflow]" PROPERTIES LABELS "diag;diag-workflow")

# engine/foundation/core/src/text_fold_table.inc is scripts/gen-text-fold.py's output from the Unicode Character
# Database Python carries; the covered blocks read the same from Python 3.12 (UCD 15.0) on.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME crd-core-text-fold-drift
             COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/gen-text-fold.py" --check
                     --root "${CMAKE_SOURCE_DIR}")
    set_tests_properties(crd-core-text-fold-drift PROPERTIES TIMEOUT 120 ENVIRONMENT "PYTHONUTF8=1")
endif()
