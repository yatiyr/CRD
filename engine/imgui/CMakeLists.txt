find_package(Vulkan REQUIRED)

# Third-party imgui sources compiled in isolation so they never inherit
# crd-warnings (/W4 /WX). A separate OBJECT library is the only way to
# prevent the D9025 "overriding /W4 with /w" cl.exe command-line warning —
# set_source_files_properties appends flags AFTER /W4, which always triggers
# D9025; the object library approach means /W4 is never on the command line
# for these files in the first place.
add_library(crd-imgui-vendor OBJECT
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(crd-imgui-vendor SYSTEM PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_link_libraries(crd-imgui-vendor PRIVATE
    Vulkan::Vulkan
    glfw
)

target_compile_options(crd-imgui-vendor PRIVATE
    $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>
)

target_compile_definitions(crd-imgui-vendor PRIVATE GLFW_INCLUDE_NONE)

set_target_properties(crd-imgui-vendor PROPERTIES
    # Skip clang-tidy on third-party imgui sources. SKIP_LINTING is the
    # CMake-3.27+ way; CXX_CLANG_TIDY="" works on every CMake ≥ 3.6 and is
    # what actually disables tidy on CI runners with older CMake versions.
    SKIP_LINTING ON
    CXX_CLANG_TIDY ""
    POSITION_INDEPENDENT_CODE ON
)

# Cerid imgui wrapper — our own sources only; vendor objects folded in.
set(CRDI_GUI_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/imgui.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/imgui_layer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/log_channel.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/settings.hpp
)

add_library(crd-imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_layer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp
    ${CRDI_GUI_HEADERS}
    $<TARGET_OBJECTS:crd-imgui-vendor>
)

target_include_directories(crd-imgui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    SYSTEM PRIVATE
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
)

target_link_libraries(crd-imgui
    PUBLIC
        crd-app
        crd-config
        crd-rhi
        crd-rhi-vulkan
        crd-platform
        crd-log
    PRIVATE
        Vulkan::Vulkan
        glfw
)
target_compile_features(crd-imgui PUBLIC cxx_std_20)
target_compile_definitions(crd-imgui PRIVATE GLFW_INCLUDE_NONE)
crd_enable_default_pch(crd-imgui)
