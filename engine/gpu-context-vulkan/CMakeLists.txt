# crd-gpu-context-vulkan — Phase 3.1.6 v17-i-a (ADR-0099).
#
# The headless Vulkan compute context: creates a VkInstance/VkDevice (compute queue + coopmat2 feature chain, guarded)
# with no surface/swapchain. Implements crd::gpu::VulkanGpuContext. This is the compute foundation kir-vulkan migrates
# onto (v17-i-b), decoupled from the rendering RHI.

# Locate the Vulkan SDK (mirrors engine/rhi-vulkan/CMakeLists.txt).
if(WIN32 AND NOT DEFINED ENV{VULKAN_SDK})
    file(GLOB _crd_vulkan_sdk_dirs LIST_DIRECTORIES true "C:/VulkanSDK/*")
    list(SORT _crd_vulkan_sdk_dirs COMPARE NATURAL ORDER ASCENDING)
    list(REVERSE _crd_vulkan_sdk_dirs)
    foreach(_sdk_dir IN LISTS _crd_vulkan_sdk_dirs)
        if(EXISTS "${_sdk_dir}/Include/vulkan/vulkan.h")
            set(ENV{VULKAN_SDK} "${_sdk_dir}")
            break()
        endif()
    endforeach()
endif()
if(DEFINED ENV{VULKAN_SDK})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
endif()
find_package(Vulkan REQUIRED)

# ADR-0103 / D-008: this backend OWNS the Vulkan-target shading-language compilers (moved out of crd-shader). shaderc
# (GLSL→SPIR-V) and dxc (HLSL→SPIR-V) headers; both libraries are loaded DYNAMICALLY at runtime, so no link dependency.
# dxc is optional — absent (e.g. Linux without dxc-dev) ⇒ CRD_HAS_DXC=0 and compile_hlsl_to_spirv returns ok=false.
find_path(CRD_SHADERC_INCLUDE_DIR shaderc/shaderc.h HINTS "$ENV{VULKAN_SDK}/Include")
if(NOT CRD_SHADERC_INCLUDE_DIR)
    message(FATAL_ERROR "shaderc not found; required for crd-gpu-context-vulkan (GLSL->SPIR-V)")
endif()

find_path(CRD_DXC_INCLUDE_DIR dxc/dxcapi.h HINTS "$ENV{VULKAN_SDK}/Include")
if(CRD_DXC_INCLUDE_DIR)
    set(CRD_HAS_DXC TRUE)
    message(STATUS "[crd-gpu-context-vulkan] dxc headers found; compile_hlsl_to_spirv enabled")
else()
    set(CRD_HAS_DXC FALSE)
    message(STATUS "[crd-gpu-context-vulkan] dxc headers NOT found; compile_hlsl_to_spirv returns stub failure")
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-gpu-context-vulkan STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-gpu-context-vulkan
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CRD_SHADERC_INCLUDE_DIR}"
)

if(CRD_HAS_DXC)
    target_include_directories(crd-gpu-context-vulkan PRIVATE "${CRD_DXC_INCLUDE_DIR}")
    target_compile_definitions(crd-gpu-context-vulkan PRIVATE CRD_HAS_DXC=1)
else()
    target_compile_definitions(crd-gpu-context-vulkan PRIVATE CRD_HAS_DXC=0)
endif()

target_link_libraries(crd-gpu-context-vulkan
    PUBLIC
        crd-gpu-context
        crd-containers
        Vulkan::Vulkan
    PRIVATE
        crd-core
        crd-memory     # default_allocator() for the compile entry points
        crd-platform   # DynamicLibrary (shaderc/dxc load) + read_file_binary of the cooked <name>.comp.spv
        crd-kir        # ADR-0103 / D-008 C1-c: create_program(KGraph, KEntry) emits GLSL via the crd-kir emitter (acyclic edge)
)

target_compile_features(crd-gpu-context-vulkan PUBLIC cxx_std_20)
crd_enable_default_pch(crd-gpu-context-vulkan)
