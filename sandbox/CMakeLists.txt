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
    crd-draw                 # RET-6 pt 4: the debug-draw scene (grid + shapes) on gpu-context — rhi-free since pt 3
    crd-imgui                # RET-5: ImGuiGpuBackend (render) + the vendored ImGui_ImplGlfw (input)
    crd-gpu-context          # ADR-0105: the ONE graphics layer
    crd-gpu-context-vulkan
    crd-kir                  # the CKIR canvas scene (KGraph → create_program)
    crd-anim                 # GEO-8: SKEL/ANIM loaders (the animated ring)
    crd-timeline             # GEO-9: the camera-shot timeline (rational-time automation drives the crane move)
    crd-scene                # GEO-7: the ADR-0050 World the imported scene instantiates into
    crd-scene-render         # GEO-7: chunk-grain extract/cull/submit through gpu-context
    crd-resources            # GEO-7: the mounted demo PACK (mesh + PBRM loaders)
    crd-geometry-spatial     # GEO-7: OctreeBuildOptions for the SpatialBVHIndex configure
    crd-jobs
    crd-log
    crd-memory
    crd-perf       # D-003 v0h: profiler substrate
    crd-perf-ui    # D-003 v0h: profiler ImGui frontend (ProfilerPanel)
    glfw                     # RET-5: glfwGetWin32Window — the native HWND for create_present_surface
    # ⛔ crd-rhi / crd-rhi-vulkan / crd-renderer / crd-shader* — RETIRED (ADR-0105; deleted at RET-8).
)
target_compile_definitions(crd-sandbox PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(crd-sandbox)

# ── crd-gizmo-probe ─────────────────────────────────────────────────────────
# REN-39: the axis triad, CLOSE UP — a second, small sandbox whose only subject is the debug-draw overlay, on the
# SAME path (authored frame graph + authored techniques through SceneRenderer, overlay woven in as a pass). It
# lives in this directory on purpose: both executables land in the same build folder, so `executable_dir()`
# resolves the SAME cooked demo pack and neither needs its own cook step.
add_executable(crd-gizmo-probe
    src/gizmo_probe.cpp
)
target_include_directories(crd-gizmo-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(crd-gizmo-probe PRIVATE
    crd-app
    crd-draw
    crd-gpu-context
    crd-gpu-context-vulkan
    crd-kir
    crd-scene
    crd-scene-render
    crd-resources
    crd-geometry-spatial
    crd-jobs
    crd-log
    crd-memory
    glfw
)
target_compile_definitions(crd-gizmo-probe PRIVATE
    CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    CRD_DEMO_ASSETS_REL_PACK="assets/cooked/demo_assets.crdr"
)
crd_enable_default_pch(crd-gizmo-probe)

# REN-39-D / RAF-12: the D3D12 backend (`--backend dx12`) is Windows-only — `crd-gpu-context-dx12` is not built on
# other platforms, and the sandbox/probe guard their DX12 code paths with `#ifdef _WIN32`. Link it in only on Windows,
# so both executables build (Vulkan-only) on Linux/macOS.
if(WIN32)
    target_link_libraries(crd-sandbox     PRIVATE crd-gpu-context-dx12)
    target_link_libraries(crd-gizmo-probe PRIVATE crd-gpu-context-dx12)
endif()

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
    "${DEMO_ASSETS_SRC_DIR}/*.stl"
    "${DEMO_ASSETS_SRC_DIR}/*.obj"
    "${DEMO_ASSETS_SRC_DIR}/*.ply"
    "${DEMO_ASSETS_SRC_DIR}/*.3mf"
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
add_dependencies(crd-gizmo-probe cook-demo-assets)

# RET-6 (ADR-0105): the crd-draw cooked-GLSL shader pack is RETIRED — draw shaders are CKIR graphs compiled at
# init; nothing to cook or copy.

# Sandbox computes the pack path at runtime as `executable_dir() / kDemoAssetsRelPack`.
# The relative form is exposed as a compile definition so the C++ side stays
# free of a hardcoded string.
target_compile_definitions(crd-sandbox PRIVATE
    CRD_DEMO_ASSETS_REL_PACK="assets/cooked/demo_assets.crdr"
)

# RET-8: the rhi ImGuiLayer's config (imgui_layer.toml) died with it — nothing to copy.
