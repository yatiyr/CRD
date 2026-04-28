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

add_executable(smoke_rhi_api
    examples/smoke_rhi_api.cpp
)

target_link_libraries(smoke_rhi_api PRIVATE
    crd-core
    crd-log
    crd-rhi
)
crd_enable_default_pch(smoke_rhi_api)

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
