# crd-shader-vulkan — ADR-0103 / D-008 C2-e. The bridge between the portable Effect runtime (crd-shader, which names no
# shading language) and the Vulkan GLSL→SPIR-V compiler (crd-gpu-context-vulkan). Provides the injected
# `crd::shader::ISpirvCompiler` (`create_vulkan_spirv_compiler`) that `crd::shader::create_runtime` needs. Linking BOTH
# here keeps neither crd-shader nor crd-gpu-context-vulkan dependent on the other (gpu-context stays rhi-free).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-shader-vulkan STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-shader-vulkan
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-shader-vulkan
    PUBLIC
        crd-shader              # the ISpirvCompiler interface + Stage
    PRIVATE
        crd-gpu-context-vulkan  # crd::gpu::compile_glsl_to_spirv (the actual compiler)
        crd-gpu-context         # crd::gpu::ShaderStage
        crd-memory              # default_allocator()
)

target_compile_features(crd-shader-vulkan PUBLIC cxx_std_20)
crd_enable_default_pch(crd-shader-vulkan)
