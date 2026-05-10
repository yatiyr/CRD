crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-draw STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-draw
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# d0 deps:
#   crd-core / crd-memory / crd-containers / crd-math: data model + shapes
#   crd-rhi / crd-renderer / crd-shader / crd-resources: GPU pipeline + overlay pass
# d3 adds crd-scene (DebugVizComponent + DebugVizSystem + VisualizerRegistry
#   integrate with the ECS World/ISystem/SchedulePhase machinery; new
#   one-way edge per CLAUDE.md dep graph review). d2 ImGui-projected text
#   adds crd-imgui later.
target_link_libraries(crd-draw
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-rhi
        crd-renderer
        crd-shader
        crd-resources
        crd-scene
    PRIVATE
        crd-log
)

target_compile_features(crd-draw PUBLIC cxx_std_20)
crd_enable_default_pch(crd-draw)

# ── draw shader pack ──────────────────────────────────────────────────────
# Cooks engine/draw/shaders/*.glsl into a single CRDR pack
# `<bin>/assets/cooked/draw_shaders.crdr`. Consumers (sandbox, smoke tests,
# eventually the application's main asset cooker run) mount it at startup
# via `crd::draw::init(rm, pack_path)`. ADR-0066 §10.
#
# The pack lives next to consumer executables. For shipping builds the
# application's own asset cooker step would consume engine/draw/shaders
# directly + bake into the application's pack — no separate runtime mount
# needed. d0 ships the standalone-pack flavour as the default.

set(DRAW_SHADERS_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(DRAW_SHADERS_OUT_DIR "${CMAKE_BINARY_DIR}/assets/cooked")
set(DRAW_SHADERS_PACK    "${DRAW_SHADERS_OUT_DIR}/draw_shaders.crdr")

file(GLOB DRAW_SHADER_SOURCES CONFIGURE_DEPENDS
    "${DRAW_SHADERS_SRC_DIR}/*.vert.glsl"
    "${DRAW_SHADERS_SRC_DIR}/*.frag.glsl"
    "${DRAW_SHADERS_SRC_DIR}/*.comp.glsl"
)

add_custom_command(
    OUTPUT  "${DRAW_SHADERS_PACK}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DRAW_SHADERS_OUT_DIR}"
    COMMAND $<TARGET_FILE:asset_cooker> cook --root "${DRAW_SHADERS_SRC_DIR}" --out "${DRAW_SHADERS_PACK}"
    DEPENDS asset_cooker ${DRAW_SHADER_SOURCES}
    COMMENT "Cooking crd-draw shaders -> ${DRAW_SHADERS_PACK}"
    VERBATIM
)

add_custom_target(crd-draw-shaders DEPENDS "${DRAW_SHADERS_PACK}")
add_dependencies(crd-draw crd-draw-shaders)

# Expose pack path to consumers via a compile definition.
target_compile_definitions(crd-draw PUBLIC
    CRD_DRAW_SHADERS_PACK_PATH="${DRAW_SHADERS_PACK}"
)
