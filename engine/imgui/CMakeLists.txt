find_package(Vulkan REQUIRED)

# Third-party imgui sources compiled in isolation so they never inherit
# crd-warnings (/W4 /WX). A separate OBJECT library is the only way to
# prevent the D9025 "overriding /W4 with /w" cl.exe command-line warning —
# set_source_files_properties appends flags AFTER /W4, which always triggers
# D9025; the object library approach means /W4 is never on the command line
# for these files in the first place.
add_library(crd-imgui-vendor OBJECT
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(crd-imgui-vendor SYSTEM PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

# Phase 3.1.6 v1g-3 (2026-05-21) — disable IPO/LTCG on the vendored imgui
# objects. MSVC's LTCG link recurrently emits a C1001 internal compiler error
# in `link!DllGetObjHandler()` at `imgui.cpp` call-sites when imgui's /GL
# objects are whole-program-optimised into a consumer exe (seen on
# win-shipping linking crd-sandbox-showcase-tests). imgui is third-party debug
# UI — there is no engine cross-TU inlining value in LTCG-ing it. Disabling IPO
# un-`/GL`-tags imgui's objects so they leave every consumer's LTCG working
# set, breaking the ICE chain; engine LTCG benefits are unaffected. Same
# precedent as zstd_static + crd-jobs (root CMakeLists.txt §270).
set_target_properties(crd-imgui-vendor PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

target_link_libraries(crd-imgui-vendor PRIVATE
    Vulkan::Vulkan
    glfw
)

target_compile_options(crd-imgui-vendor PRIVATE
    $<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>
)

target_compile_definitions(crd-imgui-vendor PRIVATE GLFW_INCLUDE_NONE)

set_target_properties(crd-imgui-vendor PROPERTIES
    # Skip clang-tidy on third-party imgui sources. SKIP_LINTING is the
    # CMake-3.27+ way; CXX_CLANG_TIDY="" works on every CMake ≥ 3.6 and is
    # what actually disables tidy on CI runners with older CMake versions.
    SKIP_LINTING ON
    CXX_CLANG_TIDY ""
    POSITION_INDEPENDENT_CODE ON
)

# Cerid imgui wrapper — our own sources only; vendor objects folded in.
set(CRDI_GUI_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/imgui.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/imgui_layer.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/log_channel.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/settings.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/crd/imgui/unit_preferences_inspector.hpp  # v0d-6
)

add_library(crd-imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_layer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/imgui_gpu_backend.cpp # RET-5: the render backend on gpu-context (ADR-0105)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log_channel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/settings.cpp
    ${CRDI_GUI_HEADERS}
    $<TARGET_OBJECTS:crd-imgui-vendor>
)

# CI-1 (2026-07-23): SYSTEM must be the FIRST argument of its own call — mid-list it parses as a literal
# include dir named "SYSTEM" and the imgui dirs silently lose -isystem (GCC then -Werror's imgui's own headers).
target_include_directories(crd-imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(crd-imgui SYSTEM PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_link_libraries(crd-imgui
    PUBLIC
        crd-app
        crd-config
        crd-rhi                # ⛔ FROZEN half (ImGuiLayer) — dies with the sandbox flip completion (RET-7)
        crd-rhi-vulkan         # ⛔ FROZEN half
        crd-gpu-context        # RET-5: ImGuiGpuBackend — the render backend on the ONE graphics layer
        crd-gpu-context-vulkan # RET-5
        crd-platform
        crd-log
        crd-units  # v0d-6: unit_preferences_inspector consumes UnitPreferences
    PRIVATE
        Vulkan::Vulkan
        glfw
)
target_compile_features(crd-imgui PUBLIC cxx_std_20)
target_compile_definitions(crd-imgui PRIVATE GLFW_INCLUDE_NONE)
crd_enable_default_pch(crd-imgui)
