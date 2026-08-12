# crd-frame-cook — REN-36 (D-007 row 139): the AUTHORABLE FRAME GRAPH asset.
# Parses + validates `.frame.toml` and cooks it to a canonical `.crdr` blob. Depends on crd-gpu-context ONLY for
# the API-NEUTRAL enums the asset reuses (FgImageFormat, DepthCompare) — the asset can therefore never name a
# backend concept, which is what makes the "one asset, both backends, bit-identical" contract structural rather
# than a promise. One-way edge: frame-cook -> {gpu-context, containers, memory}; nothing depends back on it.

if(TARGET tomlplusplus::tomlplusplus)
    set(_crd_toml_target tomlplusplus::tomlplusplus)
else()
    set(_crd_toml_target tomlplusplus)
endif()

add_library(crd-frame-cook
    src/frame_asset.cpp
    src/frame_emit.cpp      # REN-36.2: description -> .frame.toml (the editor round-trip)
    src/frame_runtime.cpp   # REN-36.2: execute a cooked graph through IFrameGraph
    src/frame_compose.cpp   # REN-37.6: SUBGRAPHS + INJECTION POINTS, flattened away before build()
    src/viewport.cpp        # REN-37.8/37.9: the FRAME-LEVEL graph composer + the budgeted viewport scheduler
    src/frame_template_bridge.cpp # RAF-8 (ADR-0106): FrameGraphDesc -> render-graph FrameGraphTemplate load bridge
    src/frame_ceir.cpp      # CEIR-15a (ADR-0127): FrameGraphDesc <-> ceir.frame converter (§126 step 1/2)
)

target_include_directories(crd-frame-cook PUBLIC include)

target_link_libraries(crd-frame-cook
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-gpu-context
        crd-render-asset-core # RAF-8: the shared diagnostics + binding vocabulary
        crd-render-pass       # RAF-8: the executor registry + typed payload the bridge fills
        crd-render-graph      # RAF-8 (ADR-0106): the single live runtime the bridge targets (ACYCLIC — render-graph
                              #        does not depend back on frame-cook)
        crd-ceir         # CEIR-15a (ADR-0127): the ceir.frame dialect the converter targets. ACYCLIC — crd-ceir is
                         #        host-only + depends on NONE of its providers (ADR-0109 §4.2), so this edge adds no cycle.
        crd-ceir-gpu     # CEIR-16-3c (ADR-0109 §4.2): build_fullscreen_ceir + LoweredCommand — the per-pass replay plans
                         #        built at load (FramePlans). ACYCLIC — ceir-gpu does not depend back on frame-cook.
        crd-log          # REN-36.2: a fallback must be REPORTED, never silent
    PRIVATE
        ${_crd_toml_target}
        crd-warnings
)

# toml++ in EXCEPTIONS-FREE mode — the repo-wide setting (crd-config does the same). Without it toml::parse
# returns a table and THROWS instead of returning a checkable parse_result, and `ParseFailed` becomes
# unreachable-by-return. The cooker must reject malformed TOML by RETURN VALUE, never by exception.
target_compile_definitions(crd-frame-cook PRIVATE TOML_EXCEPTIONS=0)
target_compile_features(crd-frame-cook PUBLIC cxx_std_20)
crd_enable_default_pch(crd-frame-cook)
