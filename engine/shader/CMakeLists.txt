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

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-shader STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-shader
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CRD_SHADERC_INCLUDE_DIR}"
)

target_link_libraries(crd-shader
    PUBLIC
        crd-core
        crd-containers
        crd-rhi
    PRIVATE
        crd-platform
)

target_compile_features(crd-shader PUBLIC cxx_std_20)
crd_enable_default_pch(crd-shader)
