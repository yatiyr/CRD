# GEO-3 stage 2: + test_png / test_tga_bmp / test_jpeg (the owned LDR codec family gates)
crd_collect_sources(TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")

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
    crd-gpu-context          # ADR-0103: ShaderStage
    crd-gpu-context-vulkan   # ADR-0103: GLSL→SPIR-V round-trip (was crd-shader::compile_glsl)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-resources-tests)

catch_discover_tests(crd-resources-tests)
