find_package(Vulkan REQUIRED)

set(CRDI_GUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_layer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp
)

set(CRDI_GUI_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/imgui.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/imgui_layer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/log_channel.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/settings.hpp
)

add_library(crd-imgui STATIC ${CRDI_GUI_SOURCES} ${CRDI_GUI_HEADERS})

target_include_directories(crd-imgui
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
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
crd_enable_default_pch(crd-imgui)
