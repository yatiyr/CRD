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
    crd-geometry-primitives
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

add_executable(smoke_resources_async
    examples/smoke_resources_async.cpp
)

target_link_libraries(smoke_resources_async PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-jobs
    crd-resources
)
crd_enable_default_pch(smoke_resources_async)

add_executable(smoke_resources_reload
    examples/smoke_resources_reload.cpp
)

target_link_libraries(smoke_resources_reload PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
)
crd_enable_default_pch(smoke_resources_reload)

add_executable(smoke_resources_stream
    examples/smoke_resources_stream.cpp
)

target_link_libraries(smoke_resources_stream PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-jobs
    crd-resources
)
crd_enable_default_pch(smoke_resources_stream)

add_executable(smoke_resources_render
    examples/smoke_resources_render.cpp
)

target_link_libraries(smoke_resources_render PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
    crd-shader
    crd-renderer
)
crd_enable_default_pch(smoke_resources_render)

add_executable(smoke_texture
    examples/smoke_texture.cpp
)

target_link_libraries(smoke_texture PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
    crd-renderer
)
crd_enable_default_pch(smoke_texture)

add_executable(smoke_mesh
    examples/smoke_mesh.cpp
)

target_link_libraries(smoke_mesh PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-platform
    crd-resources
    crd-renderer
)
crd_enable_default_pch(smoke_mesh)

add_executable(smoke_material
    examples/smoke_material.cpp
)

target_link_libraries(smoke_material PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-platform
    crd-resources
    crd-renderer
    crd-shader
)
crd_enable_default_pch(smoke_material)

add_executable(smoke_meshgen
    examples/smoke_meshgen.cpp
)

target_link_libraries(smoke_meshgen PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-renderer
    crd-meshgen
)
crd_enable_default_pch(smoke_meshgen)

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

add_executable(smoke_asset_import
    examples/smoke_asset_import.cpp
)

target_link_libraries(smoke_asset_import PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-rhi
    crd-rhi-vulkan
    crd-renderer
)
crd_enable_default_pch(smoke_asset_import)

add_executable(smoke_depth_prepass
    examples/smoke_depth_prepass.cpp
)

target_link_libraries(smoke_depth_prepass PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-rhi
    crd-rhi-vulkan
    crd-renderer
)
crd_enable_default_pch(smoke_depth_prepass)

add_executable(smoke_hesap_substrate
    examples/smoke_hesap_substrate.cpp
)

target_link_libraries(smoke_hesap_substrate PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_substrate)

add_executable(smoke_hesap_blas1
    examples/smoke_hesap_blas1.cpp
)

target_link_libraries(smoke_hesap_blas1 PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_blas1)

add_executable(smoke_hesap_blas2
    examples/smoke_hesap_blas2.cpp
)

target_link_libraries(smoke_hesap_blas2 PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_blas2)

add_executable(smoke_hesap_blas3
    examples/smoke_hesap_blas3.cpp
)

target_link_libraries(smoke_hesap_blas3 PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_blas3)

add_executable(bench_hesap_gemm
    examples/bench_hesap_gemm.cpp
)

target_link_libraries(bench_hesap_gemm PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(bench_hesap_gemm)

add_executable(bench_hesap_gemm_parallel
    examples/bench_hesap_gemm_parallel.cpp
)

target_link_libraries(bench_hesap_gemm_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-jobs
)
crd_enable_default_pch(bench_hesap_gemm_parallel)

# Reference-class shootout — built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
# Eigen 3.4 + OpenBLAS are fetched via CPM at configure time.
if(CRD_BUILD_HESAP_VS_REFERENCE)
    add_executable(bench_hesap_gemm_vs_reference
        examples/bench_hesap_gemm_vs_reference.cpp
    )
    target_link_libraries(bench_hesap_gemm_vs_reference PRIVATE
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-hesap
        crd-hesap-dense
        crd-jobs
        eigen_headers
        openblas
    )
    # OpenBLAS does NOT export BUILD_INTERFACE include dirs (only INSTALL),
    # and its CMakeLists writes the generated cblas.h + openblas_config.h to
    # the TOP-LEVEL ${CMAKE_BINARY_DIR}, not the CPM sub-build dir. Hard-code
    # those locations explicitly.
    target_include_directories(bench_hesap_gemm_vs_reference SYSTEM PRIVATE
        "${OpenBLAS_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_BINARY_DIR}/generated"
        "${CMAKE_BINARY_DIR}/_deps/openblas-build"
    )
    # Eigen's MatrixXd*MatrixXd parallelism in 3.4 requires OpenMP in the
    # CALLER's TU — `#pragma omp parallel` is expanded inline. Without this
    # Eigen silently runs serial regardless of setNbThreads(). Verified
    # at bench-start via Eigen::nbThreads() readback.
    find_package(OpenMP COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(bench_hesap_gemm_vs_reference PRIVATE OpenMP::OpenMP_CXX)
        message(STATUS "[bench_hesap_gemm_vs_reference] OpenMP enabled for Eigen-MT")
    else()
        message(WARNING "[bench_hesap_gemm_vs_reference] OpenMP NOT found — Eigen will run SERIAL")
    endif()
    # Disable warnings-as-errors for this target (Eigen + OpenBLAS headers fire
    # many warnings we have no control over). target_compile_options PRIVATE
    # appears BEFORE linked-library INTERFACE flags, so /WX from crd-warnings
    # would override our /WX-. Source-file property comes LAST → wins.
    if(MSVC)
        set_source_files_properties(examples/bench_hesap_gemm_vs_reference.cpp
            PROPERTIES COMPILE_FLAGS "/WX- /wd4242 /wd4244 /wd4267 /wd4127 /wd4146")
    else()
        set_source_files_properties(examples/bench_hesap_gemm_vs_reference.cpp
            PROPERTIES COMPILE_FLAGS "-Wno-error")
    endif()
    # PCH conflicts with eigen + cblas system headers; skip PCH for this target.

    # ---- BLAS L1 shootout ----
    add_executable(bench_hesap_blas1_vs_reference
        examples/bench_hesap_blas1_vs_reference.cpp
    )
    target_link_libraries(bench_hesap_blas1_vs_reference PRIVATE
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-hesap
        crd-hesap-dense
        eigen_headers
        openblas
    )
    target_include_directories(bench_hesap_blas1_vs_reference SYSTEM PRIVATE
        "${OpenBLAS_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_BINARY_DIR}/generated"
        "${CMAKE_BINARY_DIR}/_deps/openblas-build"
    )
    if(OpenMP_CXX_FOUND)
        target_link_libraries(bench_hesap_blas1_vs_reference PRIVATE OpenMP::OpenMP_CXX)
    endif()
    set_source_files_properties(examples/bench_hesap_blas1_vs_reference.cpp PROPERTIES
        COMPILE_FLAGS "/WX- /wd4242 /wd4244 /wd4267 /wd4127 /wd4146")

    # ---- BLAS L2 shootout ----
    add_executable(bench_hesap_blas2_vs_reference
        examples/bench_hesap_blas2_vs_reference.cpp
    )
    target_link_libraries(bench_hesap_blas2_vs_reference PRIVATE
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-hesap
        crd-hesap-dense
        eigen_headers
        openblas
    )
    target_include_directories(bench_hesap_blas2_vs_reference SYSTEM PRIVATE
        "${OpenBLAS_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
        "${CMAKE_BINARY_DIR}/generated"
        "${CMAKE_BINARY_DIR}/_deps/openblas-build"
    )
    if(OpenMP_CXX_FOUND)
        target_link_libraries(bench_hesap_blas2_vs_reference PRIVATE OpenMP::OpenMP_CXX)
    endif()
    set_source_files_properties(examples/bench_hesap_blas2_vs_reference.cpp PROPERTIES
        COMPILE_FLAGS "/WX- /wd4242 /wd4244 /wd4267 /wd4127 /wd4146")
endif()
