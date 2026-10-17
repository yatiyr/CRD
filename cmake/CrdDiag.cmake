# DIAG.0 detector specimens. Contract: docs/design/runtime-diagnostics.md (shared acceptance rules).
#
# crd_diag_specimen(<name> SOURCES <files> [SANITIZER]) builds a tiny standalone
# executable the specimen harness (crd-diag-harness) runs as a bounded child. Each
# specimen is stamped with a compile-time identity (CRD_DIAG_SPECIMEN_ID = the
# target name) so the harness can detect a mismatched binary, and it links nothing
# from the engine -- a specimen must be minimal and self-contained.
include_guard(GLOBAL)

function(crd_diag_specimen name)
    cmake_parse_arguments(ARG "SANITIZER" "" "SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "crd_diag_specimen(${name}) needs SOURCES <files>")
    endif()
    add_executable("${name}" ${ARG_SOURCES})
    target_compile_definitions("${name}" PRIVATE "CRD_DIAG_SPECIMEN_ID=\"${name}\"")
    target_compile_features("${name}" PRIVATE cxx_std_20)
    # A specimen is never part of the default build fan-out; the harness tests that
    # need it depend on it explicitly and receive its path as a compile definition.
    set_target_properties("${name}" PROPERTIES EXCLUDE_FROM_ALL TRUE)
endfunction()

# crd_diag_fatal_tests(<specimen> PREFIX <test-prefix> OWNER <test-executable> MODES <mode>...) registers one CTest
# per mode of a contract-fatal specimen (tests/support/diag/specimens/expected_fatal.hpp): `<prefix>-<mode>` runs the
# specimen with the mode and passes only on its `CRD_DIAG_FATAL_OK <mode>` line, which the specimen prints when the
# armed CRD_FATAL fired. Another assert, a crash, a hang or a clean return lacks that line and fails. The specimen gets
# the shared specimen headers and is built with the owning test executable, so every lane that builds the tests runs
# the modes.
function(crd_diag_fatal_tests specimen)
    cmake_parse_arguments(ARG "" "PREFIX;OWNER" "MODES" ${ARGN})
    if(NOT ARG_PREFIX OR NOT ARG_OWNER OR NOT ARG_MODES)
        message(FATAL_ERROR "crd_diag_fatal_tests(${specimen}) needs PREFIX, OWNER and MODES")
    endif()
    target_include_directories("${specimen}" PRIVATE "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens")
    add_dependencies("${ARG_OWNER}" "${specimen}")
    foreach(mode IN LISTS ARG_MODES)
        add_test(NAME "${ARG_PREFIX}-${mode}" COMMAND "$<TARGET_FILE:${specimen}>" "${mode}")
        set_tests_properties("${ARG_PREFIX}-${mode}" PROPERTIES
            PASS_REGULAR_EXPRESSION "CRD_DIAG_FATAL_OK ${mode}"
            FAIL_REGULAR_EXPRESSION "CRD_DIAG_FATAL_OTHER"
            TIMEOUT 60)
    endforeach()
endfunction()
