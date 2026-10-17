# GEO-3 stage 2: + test_png / test_tga_bmp / test_jpeg (the owned LDR codec family gates)
crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")
# The fuzz target is its own executable (fuzz/CMakeLists.txt), never a Catch2 source.
list(FILTER TEST_SOURCES EXCLUDE REGEX "/fuzz/")

add_executable(crd-resources-tests ${TEST_SOURCES})

target_link_libraries(crd-resources-tests PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-log
    crd-platform
    crd-resources
    crd-cooker
    crd-jobs
    crd-perf                 # DIAG.9b: the seeded FaultInjector drives load and reload faults (test_fault_injection)
    crd-gpu-context          # ADR-0103: ShaderStage
    crd-gpu-context-vulkan   # ADR-0103: GLSL→SPIR-V round-trip (was crd-shader::compile_glsl)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-resources-tests)

crd_discover_tests(crd-resources-tests)

# The resource manager's contract fatals, one child process per mode (a fatal cannot run in-process): a control block
# or a load context its bookkeeping allocator refuses leaves the manager out of memory, fatal in every build, never a
# block or a context built on null. Source and protocol: tests/support/diag/specimens/resources_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-resources-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/resources_fatal_specimen.cpp")
target_link_libraries(crd-resources-fatal-specimen PRIVATE
    crd-core crd-memory crd-containers crd-platform crd-resources)
crd_diag_fatal_tests(crd-resources-fatal-specimen PREFIX crd-resources-fatal OWNER crd-resources-tests
    MODES failed-block-refused sync-block-refused async-block-refused streamed-block-refused async-context-refused
          streamed-context-refused)

# DIAG.10a: the cooked-pack reload fuzz target and its committed corpus replay.
add_subdirectory(fuzz)
