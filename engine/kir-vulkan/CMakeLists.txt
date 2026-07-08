# crd-kir-vulkan — Phase 3.1.6 v17-b: the CKIR Vulkan backend. `KirBackendVulkan` implements the KirBackend seam over
# crd-rhi-compute (ADR-0080) + crd-shader (GLSL→SPIR-V). A SEPARATE module (ADR-0096 lean-consumer): only a consumer
# that wants the Vulkan backend links Vulkan/shaderc — `crd-kir` core stays GPU-free. ADR-0098.
#
# Edges (acyclic): crd-kir (the IR + emitter + KirBackend interface) + crd-rhi + crd-rhi-vulkan (device/dispatch) +
# crd-shader (compile the emitted GLSL) + the base substrate.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir-vulkan STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-kir-vulkan
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-kir-vulkan
    PUBLIC
        crd-kir
        crd-rhi
        crd-rhi-vulkan
        crd-shader
        crd-core
        crd-containers
        crd-memory
    PRIVATE
        crd-warnings
)

target_compile_features(crd-kir-vulkan PUBLIC cxx_std_20)
crd_enable_default_pch(crd-kir-vulkan)
