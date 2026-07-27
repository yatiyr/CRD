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
        crd-gpu-context          # ADR-0103: ShaderStage
        crd-gpu-context-vulkan   # ADR-0103: GLSL→SPIR-V (was crd-shader::compile_glsl)
        crd-math
        crd-scene
        crd-preset
        crd-profile
        crd-asset-io             # GEO-1..5: our own STL/OBJ/PLY/glTF/3MF parsers (mesh_wave1 handler)
        crd-anim                 # GEO-8: SKEL/ANIM builders (the skins/animations decompose)
        crd-timeline             # GEO-9: the TIML builder (.otio → the editorial timeline resource)
        crd-audio                # GEO-10: the ABUF/MIDI builders (.wav/.aiff/.flac/.mid)
        crd-frame-cook           # REN-38: the authored-program handlers VALIDATE + cook the five vocabularies
        crd-technique-cook
        crd-material-cook
        crd-vertex-cook
        crd-light-cook
        # stb RETIRED (GEO-3 stage 2b): texture.cpp decodes via OUR ldr_decode. cgltf + mikktspace RETIRED
        # (GEO-6: the legacy mesh.cpp DELETED — our parsers own every format; mikktspace lives on ONLY as the
        # test-side oracle, linked by the test targets themselves).
)

# v1l: scene cooker uses toml++ (same dep used by crd-config).
# TOML_EXCEPTIONS=0 selects toml++'s no-exception API (parse() returns a
# parse_result with operator! / .error() / .table()). Matches crd-config.
target_compile_definitions(crd-cooker PRIVATE TOML_EXCEPTIONS=0)
if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(crd-cooker PRIVATE tomlplusplus::tomlplusplus)
else()
    target_link_libraries(crd-cooker PRIVATE tomlplusplus)
endif()

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
