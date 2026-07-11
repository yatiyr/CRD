# ADR-0103 / D-008: the compile_glsl/compile_hlsl compilers moved to crd-gpu-context-vulkan (which owns the Vulkan-target
# compilers). crd-shader keeps Effect / reflection / runtime / material payload / the resource loaders. D-008 C2-e removed
# the LAST shaderc use (the Effect/Module RENDERING frontend, src/runtime.cpp) — it now takes an INJECTED
# `crd::shader::ISpirvCompiler` (crd-shader-vulkan wraps `crd::gpu::compile_glsl_to_spirv`). crd-shader names NO shading
# language or vendor compiler (ADR-0103 I1, `check_no_shader_language_leak`). It still links spirv-reflect — SPIR-V
# reflection is bytecode inspection, not a language.
if(WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
    file(GLOB _crd_vulkan_sdk_dirs LIST_DIRECTORIES true "C:/VulkanSDK/*")
    list(SORT _crd_vulkan_sdk_dirs COMPARE NATURAL ORDER ASCENDING)
    list(REVERSE _crd_vulkan_sdk_dirs)
    foreach(_sdk_dir IN LISTS _crd_vulkan_sdk_dirs)
        if(EXISTS "${_sdk_dir}/Source/SPIRV-Reflect/spirv_reflect.h")
            set(ENV{VULKAN_SDK} "${_sdk_dir}")
            break()
        endif()
    endforeach()
endif()

find_path(CRD_SPIRV_REFLECT_INCLUDE_DIR spirv_reflect.h HINTS "$ENV{VULKAN_SDK}/Source/SPIRV-Reflect")
find_file(CRD_SPIRV_REFLECT_SOURCE spirv_reflect.c HINTS "$ENV{VULKAN_SDK}/Source/SPIRV-Reflect")

if(NOT CRD_SPIRV_REFLECT_INCLUDE_DIR OR NOT CRD_SPIRV_REFLECT_SOURCE)
    message(FATAL_ERROR "spirv-reflect source not found; required for crd-shader 2.3c")
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
list(APPEND SOURCES "${CRD_SPIRV_REFLECT_SOURCE}")
set_source_files_properties("${CRD_SPIRV_REFLECT_SOURCE}" PROPERTIES
    LANGUAGE CXX
    SKIP_LINTING ON
    COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-shader STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-shader
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CRD_SPIRV_REFLECT_INCLUDE_DIR}"
)

target_link_libraries(crd-shader
    PUBLIC
        crd-core
        crd-containers
        crd-rhi
    PRIVATE
        crd-platform
        crd-resources
)

target_compile_features(crd-shader PUBLIC cxx_std_20)
crd_enable_default_pch(crd-shader)
