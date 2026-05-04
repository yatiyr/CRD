# ── crd-cooker static library (cook logic — linkable from tests) ────────────

crd_collect_sources(COOKER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
list(FILTER COOKER_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(crd-cooker STATIC ${COOKER_SOURCES})

target_include_directories(crd-cooker
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-cooker
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-log
        crd-platform
        crd-resources
        crd-shader
    PRIVATE
        stb
        cgltf
        mikktspace
)

target_compile_features(crd-cooker PUBLIC cxx_std_20)

crd_enable_default_pch(crd-cooker)

# ── asset_cooker executable ─────────────────────────────────────────────────

add_executable(asset_cooker
    src/main.cpp
)

target_link_libraries(asset_cooker PRIVATE
    crd-cooker
)

target_compile_features(asset_cooker PRIVATE cxx_std_20)

crd_enable_default_pch(asset_cooker)
