# crd-frame-cook tests — REN-36.1 (D-007 row 139): the authorable frame graph's schema + cooker.
add_executable(crd-frame-cook-tests
    test_frame_asset.cpp
    test_viewport.cpp   # REN-37.8/37.9: the frame-level graph composer + the budgeted viewport scheduler
    test_frame_template_bridge.cpp # RAF-8 (ADR-0106): FrameGraphDesc -> render-graph FrameGraphTemplate
    test_frame_ceir.cpp   # CEIR-15a (ADR-0127): FrameGraphDesc -> ceir.frame converter (find_frame_misuse-clean + ceir text round-trip)
)

target_link_libraries(crd-frame-cook-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform          # CEIR-15e: read_file_text for the shipped-asset fidelity gate
    crd-frame-cook
    crd-ceir              # CEIR-20b: the ceir Context for build_work_ceir(extract_work_desc(...))
    crd-ceir-gpu          # CEIR-20b: build_work_ceir + WorkBuildDesc (extract_work_desc's output)
    crd-render-pass       # RAF-8: the executor registry the bridge fills payloads against
    crd-render-graph      # RAF-8: compile() the bridged template
    crd-render-asset-core # RAF-8: the diagnostics stream
    Catch2::Catch2WithMain
)

# CEIR-15e: the SHIPPED frame assets dir, so the CEIR-path fidelity gate loads the REAL .frame.toml files.
target_compile_definitions(crd-frame-cook-tests PRIVATE CRD_FRAME_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets/frame")

crd_enable_default_pch(crd-frame-cook-tests)

catch_discover_tests(crd-frame-cook-tests)
