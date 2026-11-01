# crd-platform-window -- the window backend of the platform layer (ADR-0006): the GLFW runtime (PlatformContext), the
# OS window (Window) that feeds crd-platform's input snapshot, and the umbrella header of the whole layer. Split from
# crd-platform (OWN.1, 2026-11-01) so the OS boundary links no window library; headers and namespace are unchanged.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-platform-window STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-platform-window
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-platform-window
    PUBLIC
        crd-core
        crd-containers
        crd-platform
    PRIVATE
        crd-log
        glfw
)

target_compile_features(crd-platform-window PUBLIC cxx_std_20)
target_compile_definitions(crd-platform-window PRIVATE GLFW_INCLUDE_NONE)
crd_enable_default_pch(crd-platform-window)
