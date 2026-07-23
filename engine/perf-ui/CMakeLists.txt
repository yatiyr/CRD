# crd-perf-ui -- ImGui frontend for crd-perf (Detour D-003 v0g).
#
# Separate target so the substrate (`crd-perf`) stays ImGui-free; the
# frontend is swappable. Depends on `crd-perf` (data model) +
# `crd-imgui` (engine integration) + ImGui sources directly (the
# vendor lives behind SYSTEM PRIVATE in `crd-imgui`, so this module
# also needs to see the ImGui headers).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-perf-ui STATIC ${SOURCES} ${HEADERS})

# CI-1 (2026-07-23): SYSTEM must lead its own call — mid-list it becomes a literal "SYSTEM" include dir.
target_include_directories(crd-perf-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(crd-perf-ui SYSTEM PRIVATE
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
)

target_link_libraries(crd-perf-ui
    PUBLIC
        crd-core
        crd-perf
        crd-memory
        crd-imgui  # engine integration; vendor ImGui pulled in via SYSTEM PRIVATE above
)

target_compile_features(crd-perf-ui PUBLIC cxx_std_20)
target_compile_definitions(crd-perf-ui PRIVATE GLFW_INCLUDE_NONE)
crd_enable_default_pch(crd-perf-ui)
