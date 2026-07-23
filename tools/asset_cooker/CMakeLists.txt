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
        crd-asset-io             # GEO-1: our own STL/OBJ/PLY parsers (mesh_wave1 handler)
    PRIVATE
        # stb RETIRED (GEO-3 stage 2b): texture.cpp now decodes via OUR ldr_decode (engine/resources).
        cgltf       # legacy glTF cook path only (first-wins registry retired it; deletion pending GEO-3 close)
        mikktspace  # GEO-2's comparison ORACLE (tests) + the legacy mesh.cpp path
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

# mesh.cpp #includes mikktspace.c directly; that third-party tangent-space code
# does several `x << 32` on 32-bit ints (benign hash mixing) which trips UBSan's
# shift-exponent check on the linux-gcc-asan config. Disable only the shift check
# for this one TU (no-op when UBSan isn't active; not applicable to MSVC).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/cook_handlers/mesh.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fno-sanitize=shift>")

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
