# RET-5 pt 2 (ADR-0105): THE SANDBOX FLIP. The sandbox runs END TO END on the ONE graphics layer (gpu-context):
# windowed VulkanGpuContext → IRasterContext → present surface → ImGuiGpuBackend overlay. The frozen-stack scene
# layers (sandbox_layer / geometry_showcase / curves_showcase — built on crd-renderer's ForwardRenderPath) left the
# BUILD, not the tree: their content returns through the RET-6 draw port onto gpu-context.
add_executable(crd-sandbox
    src/main.cpp
)

# CI-1 (2026-07-23): SYSTEM must lead its own call — mid-list it becomes a literal "SYSTEM" include dir.
target_include_directories(crd-sandbox PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# RET-5: imgui.h + backends/imgui_impl_glfw.h (the platform-input half)
target_include_directories(crd-sandbox SYSTEM PRIVATE "${imgui_SOURCE_DIR}")

target_link_libraries(crd-sandbox PRIVATE
    crd-app
    crd-imgui                # RET-5: ImGuiGpuBackend (render) + the vendored ImGui_ImplGlfw (input)
    crd-gpu-context          # ADR-0105: the ONE graphics layer
    crd-gpu-context-vulkan
    crd-kir                  # the CKIR canvas scene (KGraph → create_program)
    crd-jobs
    crd-log
    crd-memory
    crd-perf       # D-003 v0h: profiler substrate
    crd-perf-ui    # D-003 v0h: profiler ImGui frontend (ProfilerPanel)
    glfw                     # RET-5: glfwGetWin32Window — the native HWND for create_present_surface
    # ⛔ crd-rhi / crd-rhi-vulkan / crd-renderer / crd-draw / crd-scene / crd-shader* — RETIRED from the sandbox
    #    (ADR-0105). crd-imgui still links the frozen rhi half transitively until RET-7 kills ImGuiLayer.
)
target_compile_definitions(crd-sandbox PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(crd-sandbox)

# ── cook-demo-assets ────────────────────────────────────────────────────────
# Cooks committed glTF/PNG demo assets from assets/source/ into a single PACK
# at <exe_dir>/assets/cooked/demo_assets.crdr (i.e. next to the executable in
# the build tree). The sandbox resolves the path at runtime via
# crd::platform::fs::executable_dir(), so the build/<preset>/sandbox/ folder is
# self-contained and shippable. The repo source tree never holds cook output.

set(DEMO_ASSETS_SRC_DIR "${CMAKE_SOURCE_DIR}/assets/source")
set(DEMO_ASSETS_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets/cooked")
set(DEMO_ASSETS_PACK    "${DEMO_ASSETS_OUT_DIR}/demo_assets.crdr")

# Glob source files explicitly so add_custom_command's DEPENDS triggers a recook
# whenever any committed source asset changes.
file(GLOB_RECURSE DEMO_ASSETS_SOURCES CONFIGURE_DEPENDS
    "${DEMO_ASSETS_SRC_DIR}/*.glb"
    "${DEMO_ASSETS_SRC_DIR}/*.gltf"
    "${DEMO_ASSETS_SRC_DIR}/*.png"
    "${DEMO_ASSETS_SRC_DIR}/*.jpg"
    "${DEMO_ASSETS_SRC_DIR}/*.jpeg"
    "${DEMO_ASSETS_SRC_DIR}/*.tga"
    "${DEMO_ASSETS_SRC_DIR}/*.bmp"
    "${DEMO_ASSETS_SRC_DIR}/*.preset.toml"
    "${DEMO_ASSETS_SRC_DIR}/*.profile.toml"
    "${DEMO_ASSETS_SRC_DIR}/*.obek.toml"
)

add_custom_command(
    OUTPUT  "${DEMO_ASSETS_PACK}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEMO_ASSETS_OUT_DIR}"
    COMMAND $<TARGET_FILE:asset_cooker> cook --root "${DEMO_ASSETS_SRC_DIR}" --out "${DEMO_ASSETS_PACK}"
    DEPENDS asset_cooker ${DEMO_ASSETS_SOURCES}
    COMMENT "Cooking sandbox demo assets → ${DEMO_ASSETS_PACK}"
    VERBATIM
)

add_custom_target(cook-demo-assets DEPENDS "${DEMO_ASSETS_PACK}")
add_dependencies(crd-sandbox cook-demo-assets)

# Copy the crd-draw shader pack next to the sandbox executable (same
# layout as demo_assets so the runtime resolves both via executable_dir()).
# Pack is produced by the `crd-draw-shaders` target in engine/draw/CMakeLists.txt.
set(SANDBOX_DRAW_PACK_OUT "${CMAKE_CURRENT_BINARY_DIR}/assets/cooked/draw_shaders.crdr")
add_custom_command(
    OUTPUT  "${SANDBOX_DRAW_PACK_OUT}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEMO_ASSETS_OUT_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_BINARY_DIR}/assets/cooked/draw_shaders.crdr" "${SANDBOX_DRAW_PACK_OUT}"
    DEPENDS crd-draw-shaders "${CMAKE_BINARY_DIR}/assets/cooked/draw_shaders.crdr"
    COMMENT "Copying crd-draw shader pack -> ${SANDBOX_DRAW_PACK_OUT}"
    VERBATIM
)
add_custom_target(sandbox-draw-pack DEPENDS "${SANDBOX_DRAW_PACK_OUT}")
add_dependencies(crd-sandbox sandbox-draw-pack)

target_compile_definitions(crd-sandbox PRIVATE
    CRD_DRAW_SHADERS_REL_PACK="assets/cooked/draw_shaders.crdr"
)

# Sandbox computes the pack path at runtime as `executable_dir() / kDemoAssetsRelPack`.
# The relative form is exposed as a compile definition so the C++ side stays
# free of a hardcoded string.
target_compile_definitions(crd-sandbox PRIVATE
    CRD_DEMO_ASSETS_REL_PACK="assets/cooked/demo_assets.crdr"
)

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
