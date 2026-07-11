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

add_library(crd-rhi-vulkan STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-rhi-vulkan
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-rhi-vulkan
    PUBLIC
        crd-rhi
        crd-platform
        crd-log
        crd-perf   # v0d profiler GPU backend (VulkanProfilerBackend implements crd::perf::IProfilerGpuBackend)
    PRIVATE
        crd-memory   # ADR-0085 S6: GpuAllocator suballocates via crd::memory::OffsetAllocator
        crd-gpu-context          # D-008 C2-b: IGpuContext / GpuBackend for create_vulkan_device_adopting
        crd-gpu-context-vulkan   # D-008 C2-b: VulkanGpuContext handle extraction (acyclic — gpu-context is foundational)
        Vulkan::Vulkan
        glfw
)

target_compile_features(crd-rhi-vulkan PUBLIC cxx_std_20)
# Vulkan structs use aggregate init ({VK_STRUCTURE_TYPE_...}) which leaves pNext
# unspecified; clang warns about this but it's intentional — Vulkan zeroes all
# unset fields. Suppress only on clang to avoid masking real issues on MSVC.
target_compile_options(crd-rhi-vulkan PRIVATE
    # Vulkan C structs are designed to be partially initialized; pNext and
    # unused flags are intentionally left zero. Suppress both the classic and
    # the designated-initializer variant of this warning for this target only.
    $<$<CXX_COMPILER_ID:Clang>:-Wno-missing-field-initializers>
    $<$<CXX_COMPILER_ID:Clang>:-Wno-missing-designated-field-initializers>
)
crd_enable_default_pch(crd-rhi-vulkan)
