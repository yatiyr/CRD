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
