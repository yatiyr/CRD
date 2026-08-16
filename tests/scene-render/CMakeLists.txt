add_executable(crd-scene-render-tests
    test_scene_render.cpp     # GEO-7: chunk-grain extract + THE partial-re-upload gate (stub raster, CPU)
    test_scene_render_gpu.cpp # GEO-7: the 10k-instance cull+draw gate on a real Vulkan device ([gpu])
    test_csm.cpp              # REN-3.2-b: CPU cascade fitting + the no-swim stabilization gates
    test_raf10_app.cpp        # RAF-10 (D-007): an APPLICATION customises the renderer ten ways, public headers only
    test_raf11_reload.cpp     # RAF-11 (D-007): dependency-aware hot reload — the reload engine + generations (device-free)
)

target_link_libraries(crd-scene-render-tests PRIVATE
    crd-frame-cook   # REN-36: the AUTHORED graph asset the CSM gate parses
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-units
    crd-platform
    crd-geometry-primitives
    crd-geometry-spatial
    crd-geometry-mesh-processing # REN-41 Stage 4: cook_cluster_dag + unpack_selected_clusters (the cluster-mesh gate oracle)
    crd-meshgen                  # CEIR-18p impostor STEP 0.5: make_icosphere — a mesh dense enough to build a real LOD chain
    crd-scene
    crd-resources
    crd-scene-render
    crd-gpu-context
    crd-gpu-context-vulkan
    crd-kir
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-render-tests)
target_compile_definitions(crd-scene-render-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # std::getenv (the drift gate's ctest env)
target_link_libraries(crd-scene-render-tests PRIVATE crd-material-cook crd-vertex-cook crd-light-cook) # REN-38: the drift gate parses all five vocabularies
target_link_libraries(crd-scene-render-tests PRIVATE crd-render-graph crd-render-pass) # RAF-10: the app's custom executor names RecordContext / PassPayload / pass_param_id directly
if(WIN32) # REN-38-F6: the DX12 twins of the advanced-family render gates (the backend exists only on Windows)
    target_link_libraries(crd-scene-render-tests PRIVATE crd-gpu-context-dx12)
endif()

# REN-38 audit: the DRIFT GATE reads the shipped authored assets — the root rides ctest, so the guard lives in
# ctest exactly like every cross-config guard (binary-direct runs SKIP rather than passing on nothing). CRD_ASSETS_DIR
# (the SHIPPED-assets root) MUST stay env-only so a binary-direct run skips instead of falsely passing.
# ⚠ SCAR (D-007 CEIR grind, 2026-08-08): a catch_discover_tests ENVIRONMENT property CANNOT carry two `;`-joined vars.
# cmake_parse_arguments flattens "A=x;B=y" into two list elements at capture, then the value is forwarded over a `-D`
# command line and re-emitted — neither `\;` NOR `\\;` survives (verified: both still generate ENVIRONMENT [==[A]==]
# [==[B]==], which set_tests_properties misparses so BOTH the 2nd var AND SKIP_RETURN_CODE are lost → RAF-10 skips and
# the skip scores ***Failed). So: keep the ONE shipped-root var here (single-var ENVIRONMENT works + preserves the
# skip-gate), and give RAF-10 its OWN app fixtures via a compiled path (CRD_RAF10_APP_ASSETS_DIR, env-overridable).
catch_discover_tests(crd-scene-render-tests
    PROPERTIES ENVIRONMENT "CRD_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets")
# RAF-10's app_assets are this test's OWN fixtures (fixed source location) — a compiled path is robust + config-invariant;
# the env var still wins if ctest/a dev sets it.
target_compile_definitions(crd-scene-render-tests PRIVATE
    CRD_RAF10_APP_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/app_assets")
