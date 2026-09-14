# Bounded fuzz targets (REPO.DEV.9). Contract: docs/design/test-instruments.md.
# crd_fuzz_target(<name> SOURCES <files> LIBRARIES <targets> CORPUS <directory>) builds the replay executable <name>
# (the target source plus tests/support/fuzz/src/replay_main.cpp) and registers the <name>-corpus CTest that replays
# every file of the committed corpus on every lane; under CRD_ENABLE_FUZZER (clang only, tree-wide coverage
# instrumentation from the root) it also builds <name>-libfuzzer from the same source with libFuzzer's driver.
include_guard(GLOBAL)

set(CRD_FUZZ_REPLAY_MAIN "${CMAKE_SOURCE_DIR}/tests/support/fuzz/src/replay_main.cpp")

function(crd_fuzz_target name)
    cmake_parse_arguments(ARG "" "CORPUS" "SOURCES;LIBRARIES" ${ARGN})
    if(NOT ARG_SOURCES OR NOT ARG_CORPUS)
        message(FATAL_ERROR "crd_fuzz_target(${name}) needs SOURCES <files> and CORPUS <directory>")
    endif()
    if(NOT IS_DIRECTORY "${ARG_CORPUS}")
        message(FATAL_ERROR "crd_fuzz_target(${name}): corpus directory ${ARG_CORPUS} does not exist; the committed "
                            "corpus is the regression set the replay test proves")
    endif()
    add_executable("${name}" ${ARG_SOURCES} "${CRD_FUZZ_REPLAY_MAIN}")
    target_link_libraries("${name}" PRIVATE crd-fuzz-harness ${ARG_LIBRARIES})
    add_test(NAME "${name}-corpus" COMMAND "${name}" "${ARG_CORPUS}")
    set_tests_properties("${name}-corpus" PROPERTIES TIMEOUT 300 LABELS "fuzz")
    if(CRD_ENABLE_FUZZER)
        add_executable("${name}-libfuzzer" ${ARG_SOURCES})
        target_link_libraries("${name}-libfuzzer" PRIVATE crd-fuzz-harness ${ARG_LIBRARIES})
        target_compile_options("${name}-libfuzzer" PRIVATE -fsanitize=fuzzer)
        target_link_options("${name}-libfuzzer" PRIVATE -fsanitize=fuzzer)
    endif()
endfunction()
