if(WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
    file(GLOB _crd_vulkan_sdk_dirs LIST_DIRECTORIES true "C:/VulkanSDK/*")
    list(SORT _crd_vulkan_sdk_dirs COMPARE NATURAL ORDER ASCENDING)
    list(REVERSE _crd_vulkan_sdk_dirs)
    foreach(_sdk_dir IN LISTS _crd_vulkan_sdk_dirs)
        if(EXISTS "${_sdk_dir}/Include/shaderc/shaderc.h")
            set(ENV{VULKAN_SDK} "${_sdk_dir}")
            break()
        endif()
    endforeach()
endif()

find_path(CRD_SHADERC_INCLUDE_DIR shaderc/shaderc.h HINTS "$ENV{VULKAN_SDK}/Include")

if(NOT CRD_SHADERC_INCLUDE_DIR)
    message(FATAL_ERROR "shaderc not found; required for crd-shader 2.3b")
endif()

# dxc API header (ships with the Vulkan SDK alongside shaderc on Windows;
# typically absent from system Vulkan installs on Linux). Header only —
# dxcompiler is loaded dynamically at runtime, no link-time dependency.
#
# **Optional** (v9e-c-dxc-spirv-dispatch follow-up after the 2026-05-19
# full-sweep failure on Linux): if dxc headers are unavailable at configure
# time, `compile_hlsl` builds as a graceful-failure stub returning
# `CompileResult{ok=false, error_message="dxc not available at build time"}`.
# This matches `compile_hlsl`'s already-documented missing-library
# fallthrough — Linux configs ship without dxc and HLSL conformance tests
# soft-skip on those configs.
find_path(CRD_DXC_INCLUDE_DIR dxc/dxcapi.h HINTS "$ENV{VULKAN_SDK}/Include")

if(CRD_DXC_INCLUDE_DIR)
    set(CRD_HAS_DXC TRUE)
    message(STATUS "[crd-shader] dxc headers found at ${CRD_DXC_INCLUDE_DIR}; compile_hlsl enabled")
else()
    set(CRD_HAS_DXC FALSE)
    message(STATUS "[crd-shader] dxc headers NOT found; compile_hlsl returns stub failure (Linux without dxc-dev)")
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
        "${CRD_SHADERC_INCLUDE_DIR}"
        "${CRD_SPIRV_REFLECT_INCLUDE_DIR}"
)

if(CRD_HAS_DXC)
    target_include_directories(crd-shader PRIVATE "${CRD_DXC_INCLUDE_DIR}")
    target_compile_definitions(crd-shader PRIVATE CRD_HAS_DXC=1)
else()
    target_compile_definitions(crd-shader PRIVATE CRD_HAS_DXC=0)
endif()

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
