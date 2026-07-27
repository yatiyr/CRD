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
# RET-6 (ADR-0105): crd-draw runs on the ONE graphics layer. crd-rhi / crd-renderer / crd-shader / crd-resources
# are GONE — the CKIR draw-shader suite replaced the cooked-GLSL pack, and gpu-context replaced the rhi renderer.
target_link_libraries(crd-draw
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-kir                # the ONE IR (the authored suite cooks into it)
        crd-vertex-cook        # REN-38-F7: the draw VS declarations cook through .crdv
        crd-material-cook      # REN-38-F7: the draw FS materials cook through .crdm
        crd-gpu-context        # IRasterContext / draw_overlay — the ONE graphics layer
        crd-gpu-context-vulkan # VulkanGpuContext::create_program (the ADR-0103 currency)
        crd-scene
    PRIVATE
        crd-log
)

target_compile_features(crd-draw PUBLIC cxx_std_20)
crd_enable_default_pch(crd-draw)

# RET-6 (ADR-0105): the cooked-GLSL shader pack is RETIRED — the draw shaders are AUTHORED assets (draw_assets.hpp)
# compiled at init through the ONE graphics layer. The GLSL sources in shaders/ stay in-tree as the porting
# reference the ckir gate names, and die with the RET-8 sweep.
