# crd-gpu-context-vulkan tests — Phase 3.1.6 v17-i-a (ADR-0099): the headless Vulkan compute context stands up on its own.
# Also hosts the CKIR GLSL-emitter compile gate (ADR-0103 / D-008): the shaderc compiler moved here from crd-shader, so
# the "emitted GLSL compiles to SPIR-V" test lives with it (and off the crd-kir link-isolation smoke, which must stay
# GPU-API-free).
add_executable(crd-gpu-context-vulkan-tests
    test_vulkan_context.cpp
    test_ckir_glsl_compile.cpp
)

target_link_libraries(crd-gpu-context-vulkan-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir                  # the CKIR IR + GLSL emitter under test
    crd-gpu-context
    crd-gpu-context-vulkan
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-gpu-context-vulkan-tests)

catch_discover_tests(crd-gpu-context-vulkan-tests)
