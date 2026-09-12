# crd-vertex-cook — REN-38-D1/D2/D3/D4 (D-007 row 141): the VERTEX PROGRAM as an authored asset.
# Parses + validates `.crdv` and COOKS it into a CKIR vertex entry — replacing `build_scene_vs_shadowed` /
# `_skinned` / `build_shadow_vs`, ~200 lines of C++ with the vertex-pull layout compiled in.
#
# ⭐ The `crd-material-cook` edge is deliberate: the displacement graph (38-D3) uses the SAME node registry a
# material does. A second node library would be two vocabularies that drift, and an author would have no way to
# know which asset understood which node.
# One-way: vertex-cook -> {material-cook, kir, containers, memory, core}; nothing depends back on it.

if(TARGET tomlplusplus::tomlplusplus)
    set(_crd_toml_target tomlplusplus::tomlplusplus)
else()
    set(_crd_toml_target tomlplusplus)
endif()

add_library(crd-vertex-cook
    src/vertex_asset.cpp
)

target_include_directories(crd-vertex-cook PUBLIC include)

target_link_libraries(crd-vertex-cook
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-kir
        crd-material-cook   # the shared node registry (38-D3 displacement)
    PRIVATE
        ${_crd_toml_target}
)

target_compile_features(crd-vertex-cook PUBLIC cxx_std_20)
# TOML_EXCEPTIONS=0: the no-exception toml++ API (parse() returns a checked result). A thrown
# parse_error unwinding while a live GPU device had hooked SEH dispatch crashed the process (F15).
target_compile_definitions(crd-vertex-cook PRIVATE TOML_EXCEPTIONS=0)

crd_enable_default_pch(crd-vertex-cook)
