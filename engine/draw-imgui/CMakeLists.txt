crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-draw-imgui STATIC ${SOURCES} ${HEADERS})

# CI-1 (2026-07-23): SYSTEM must lead its own call — mid-list it becomes a literal "SYSTEM" include dir.
target_include_directories(crd-draw-imgui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(crd-draw-imgui SYSTEM PRIVATE "${imgui_SOURCE_DIR}")

# Peer-companion module (Phase 3.1 v1a-draw d4): depends on crd-draw +
# crd-imgui. Lives outside crd-draw so headless / DAW builds that don't
# link ImGui don't drag it in transitively. Same pattern eylem-viz will
# follow at v1b.
target_link_libraries(crd-draw-imgui
    PUBLIC
        crd-core
        crd-draw
        crd-imgui
)

target_compile_features(crd-draw-imgui PUBLIC cxx_std_20)
crd_enable_default_pch(crd-draw-imgui)
