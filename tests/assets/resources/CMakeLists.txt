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

# DIAG.10a: the cooked-pack reload fuzz target and its committed corpus replay.
add_subdirectory(fuzz)
