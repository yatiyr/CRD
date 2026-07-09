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

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-gpu-context-vulkan STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-gpu-context-vulkan
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-gpu-context-vulkan
    PUBLIC
        crd-gpu-context
        crd-containers
        Vulkan::Vulkan
    PRIVATE
        crd-core
        crd-platform   # ADR-0100: read_file_binary of the cooked <name>.comp.spv in create_pipeline(name)
)

target_compile_features(crd-gpu-context-vulkan PUBLIC cxx_std_20)
crd_enable_default_pch(crd-gpu-context-vulkan)
