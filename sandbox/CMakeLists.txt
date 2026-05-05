add_executable(crd-sandbox
    src/main.cpp
    src/sandbox_layer.cpp
)

target_include_directories(crd-sandbox PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    SYSTEM "${imgui_SOURCE_DIR}"
)

target_link_libraries(crd-sandbox PRIVATE
    crd-app
    crd-config
    crd-imgui
    crd-log
    crd-math
    crd-meshgen
    crd-renderer
    crd-rhi
    crd-rhi-vulkan
    crd-shader
)
target_compile_definitions(crd-sandbox PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(crd-sandbox)

# Copy imgui config so the sandbox can find it at runtime.
set(SANDBOX_IMGUI_CONFIG "${CMAKE_SOURCE_DIR}/runtime/configs/imgui_layer.toml")
set(SANDBOX_IMGUI_CONFIG_OUT "${CMAKE_CURRENT_BINARY_DIR}/configs/imgui_layer.toml")

add_custom_command(
    OUTPUT  "${SANDBOX_IMGUI_CONFIG_OUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/configs"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SANDBOX_IMGUI_CONFIG}" "${SANDBOX_IMGUI_CONFIG_OUT}"
    DEPENDS "${SANDBOX_IMGUI_CONFIG}"
    VERBATIM
)
add_custom_target(sandbox_imgui_config DEPENDS "${SANDBOX_IMGUI_CONFIG_OUT}")
add_dependencies(crd-sandbox sandbox_imgui_config)
