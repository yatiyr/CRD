add_executable(crd-runtime
    src/main.cpp
)

target_link_libraries(crd-runtime PRIVATE
    crd-core
    crd-log
    crd-memory
)
crd_enable_default_pch(crd-runtime)

add_executable(smoke_log
    examples/smoke_log.cpp
)

target_link_libraries(smoke_log PRIVATE
    crd-core
    crd-log
)
crd_enable_default_pch(smoke_log)

add_executable(smoke_memory
    examples/smoke_memory.cpp
)

target_link_libraries(smoke_memory PRIVATE
    crd-core
    crd-log
    crd-memory
)
crd_enable_default_pch(smoke_memory)

add_executable(smoke_containers
    examples/smoke_containers.cpp
)

target_link_libraries(smoke_containers PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
)
crd_enable_default_pch(smoke_containers)

add_executable(smoke_math
    examples/smoke_math.cpp
)

target_link_libraries(smoke_math PRIVATE
    crd-core
    crd-math
)
crd_enable_default_pch(smoke_math)

add_executable(smoke_window
    examples/smoke_window.cpp
)

target_link_libraries(smoke_window PRIVATE
    crd-core
    crd-log
    crd-containers
    crd-platform
)
crd_enable_default_pch(smoke_window)

add_executable(smoke_config
    examples/smoke_config.cpp
)

target_link_libraries(smoke_config PRIVATE
    crd-core
    crd-log
    crd-config
)
crd_enable_default_pch(smoke_config)

add_executable(smoke_shader
    examples/smoke_shader.cpp
)

target_link_libraries(smoke_shader PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-shader
)
target_compile_definitions(smoke_shader PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(smoke_shader)

add_executable(smoke_renderer
    examples/smoke_renderer.cpp
)

target_link_libraries(smoke_renderer PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-rhi
    crd-rhi-vulkan
    crd-shader
    crd-renderer
)
target_compile_definitions(smoke_renderer PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(smoke_renderer)

add_executable(smoke_frame_clock
    examples/smoke_frame_clock.cpp
)

target_link_libraries(smoke_frame_clock PRIVATE
    crd-core
    crd-log
    crd-platform
)
crd_enable_default_pch(smoke_frame_clock)

add_executable(smoke_filesystem
    examples/smoke_filesystem.cpp
)

target_link_libraries(smoke_filesystem PRIVATE
    crd-core
    crd-log
    crd-platform
)
crd_enable_default_pch(smoke_filesystem)

add_executable(smoke_app
    examples/smoke_app.cpp
)

target_link_libraries(smoke_app PRIVATE
    crd-core
    crd-log
    crd-app
)
crd_enable_default_pch(smoke_app)

add_executable(smoke_imgui_overlay
    examples/smoke_imgui_overlay.cpp
)

add_dependencies(smoke_imgui_overlay smoke_rhi_vulkan_shaders)

set(SMOKE_IMGUI_CONFIGS
    "${CMAKE_SOURCE_DIR}/runtime/configs/imgui_layer.toml"
)

set(SMOKE_IMGUI_CONFIG_OUTPUTS)
foreach(config_file IN LISTS SMOKE_IMGUI_CONFIGS)
    get_filename_component(config_name "${config_file}" NAME)
    set(config_copy "${CMAKE_CURRENT_BINARY_DIR}/configs/${config_name}")
    add_custom_command(
        OUTPUT "${config_copy}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/configs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${config_file}" "${config_copy}"
        DEPENDS "${config_file}"
        VERBATIM
    )
    list(APPEND SMOKE_IMGUI_CONFIG_OUTPUTS "${config_copy}")
endforeach()

add_custom_target(smoke_imgui_configs DEPENDS ${SMOKE_IMGUI_CONFIG_OUTPUTS})
add_dependencies(smoke_imgui_overlay smoke_imgui_configs)

target_link_libraries(smoke_imgui_overlay PRIVATE
    crd-core
    crd-log
    crd-app
    crd-config
    crd-imgui
    crd-rhi
    crd-rhi-vulkan
)
crd_enable_default_pch(smoke_imgui_overlay)

add_executable(smoke_rhi_api
    examples/smoke_rhi_api.cpp
)

target_link_libraries(smoke_rhi_api PRIVATE
    crd-core
    crd-log
    crd-rhi
)
crd_enable_default_pch(smoke_rhi_api)

add_executable(smoke_jobs
    examples/smoke_jobs.cpp
)

target_link_libraries(smoke_jobs PRIVATE
    crd-core
    crd-jobs
)
crd_enable_default_pch(smoke_jobs)

add_executable(smoke_resources
    examples/smoke_resources.cpp
)

target_link_libraries(smoke_resources PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
)
crd_enable_default_pch(smoke_resources)

add_executable(smoke_rhi_vulkan_bootstrap
    examples/smoke_rhi_vulkan_bootstrap.cpp
)

set(SMOKE_RHI_VULKAN_SHADERS
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.vert"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.frag"
)
crd_compile_glsl(SMOKE_RHI_VULKAN_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders" ${SMOKE_RHI_VULKAN_SHADERS})

add_custom_target(smoke_rhi_vulkan_shaders DEPENDS ${SMOKE_RHI_VULKAN_SPV})
add_dependencies(smoke_rhi_vulkan_bootstrap smoke_rhi_vulkan_shaders)

target_link_libraries(smoke_rhi_vulkan_bootstrap PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-rhi
    crd-rhi-vulkan
)
crd_enable_default_pch(smoke_rhi_vulkan_bootstrap)
