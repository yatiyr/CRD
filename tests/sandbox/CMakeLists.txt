# crd-sandbox-showcase-tests — Phase 3.1.7 v1-close debt payment.
#
# Automated coverage of the sandbox's geometry / draw showcase render paths
# (the four GeometryShowcaseMode sub-modes + the DrawShowcase scene). The
# initial v1j-b smoke test only exercised the Physics-default boot path —
# the four showcase modes' code paths had zero automation. This binary
# closes the gap by directly invoking `render_geometry_showcase` and
# `render_draw_showcase` against a TLSF-backed RenderBuffer and asserting
# each mode emits a non-zero line count.
#
# Compiles `sandbox/src/geometry_showcase.cpp` directly (no separate library
# target — the showcase is a sandbox-internal TU). Links the same deps the
# sandbox does for the showcase to compile: geometry-* + draw + imgui (the
# `draw_geometry_showcase_imgui` function references ImGui symbols, so the
# vendor lib must link even though the tests never call it).

add_executable(crd-sandbox-showcase-tests
    test_showcase.cpp
    "${CMAKE_SOURCE_DIR}/sandbox/src/geometry_showcase.cpp"
    "${CMAKE_SOURCE_DIR}/sandbox/src/curves_showcase.cpp" # v10e: curves showcase
)

target_include_directories(crd-sandbox-showcase-tests PRIVATE
    "${CMAKE_SOURCE_DIR}/sandbox/src"
    SYSTEM "${imgui_SOURCE_DIR}" # geometry_showcase.cpp's ImGui calls
)

target_link_libraries(crd-sandbox-showcase-tests PRIVATE
    crd-core
    crd-math
    crd-memory
    crd-containers
    crd-geometry-primitives
    crd-geometry-bvh
    crd-geometry-curves           # v10e: curves substrate for CurvesShowcase
    crd-geometry-viz
    crd-draw
    crd-imgui      # transitively links imgui-vendor for the ImGui symbols
    Catch2::Catch2WithMain
)

# Phase 3.1.6 v1g-3 (2026-05-21) — NO PCH on this test exe. gcc-13 segfaults
# (internal compiler error in libc free, reported at a meaningless <cmath>
# system-header line) when compiling curves_showcase.cpp through the precompiled
# header under `-fsanitize=address,undefined` (linux-gcc-asan) — a known gcc-13
# PCH+sanitizer flake. PCH gives a test exe of this size almost nothing (the
# wins are in engine libs), so dropping it removes the ICE trigger surgically.
# (Intentionally NOT calling crd_enable_default_pch here.)

# Phase 3.1.6 v1g-3 (2026-05-21) — disable IPO/LTCG on this TEST exe. MSVC's
# LTCG link step recurrently emits a C1001 internal compiler error in
# `link!DllGetObjHandler()` when whole-program-optimising this exe (the
# geometry_showcase.cpp template-instantiation graph is the largest LTCG
# contributor). It cleared on retry every time (non-deterministic linker bug,
# not a real codegen error) — but a test exe needs NO LTO (it verifies
# correctness, not perf). Disabling IPO un-`/GL`-tags this exe's objects so they
# leave the LTCG working set, breaking the ICE chain; engine-lib LTCG coverage
# (the part that matters) is unaffected. Same precedent as zstd_static +
# crd-jobs (root CMakeLists.txt §270).
set_target_properties(crd-sandbox-showcase-tests PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

catch_discover_tests(crd-sandbox-showcase-tests)
