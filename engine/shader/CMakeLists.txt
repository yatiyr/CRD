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
