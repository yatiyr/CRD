# crd-render-graph — RAF-7 (D-007 "RAF band"; mission §7).
#
# The UNIFIED frame-graph runtime: the separated FrameGraphTemplate / CompiledFrameGraph / execution forms (compile
# once, execute many); DAG scheduling + transient aliasing + persistent/history pinning; executor RECORD functions
# that emit the RAF-2 CANONICAL command model into an ICommandEncoder (retiring the FramePassKind→verb switch), with
# declared==recorded validation. Backend-agnostic; the same graph, hand-built or authored, records the same commands.
# Deps: core / containers / memory / render-asset-core / render-pass / gpu-context.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-render-graph STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-render-graph
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-render-graph
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-render-asset-core   # dependency graph + diagnostics
        crd-render-pass         # executor schemas + payload validation
        crd-gpu-context         # ICommandEncoder + the canonical command model
        crd-ceir-gpu            # CEIR-16-3c: execute_render_lowered / RenderResolvers (the CEIR replay executor); ADR-0109 §4.2 edge
)

target_compile_features(crd-render-graph PUBLIC cxx_std_20)
crd_enable_default_pch(crd-render-graph)
