# crd-material-cook — REN-38-C1/C2/C3 (D-007 row 141): the MATERIAL as an authored asset.
# Parses + validates `.crdm` and COOKS it into CKIR — replacing `MaterialTemplate::build_surface`, a C++ function
# pointer that made inventing a material an engine edit.
#
# The `crd-kir` edge lives here for the same reason it lives in crd-technique-cook: a material's types are IR
# types, while `crd-frame-cook` stays on the API-NEUTRAL GPU enums alone.
# One-way: material-cook -> {kir, containers, memory, core}; nothing depends back on it.

if(TARGET tomlplusplus::tomlplusplus)
    set(_crd_toml_target tomlplusplus::tomlplusplus)
else()
    set(_crd_toml_target tomlplusplus)
endif()

add_library(crd-material-cook
    src/material_asset.cpp
)

target_include_directories(crd-material-cook PUBLIC include)

target_link_libraries(crd-material-cook
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-kir          # the node library (ckir_nodes) + the OpenPBR surface contract (ckir_material)
    PRIVATE
        ${_crd_toml_target}
)

target_compile_features(crd-material-cook PUBLIC cxx_std_20)
# TOML_EXCEPTIONS=0: the no-exception toml++ API (parse() returns a checked result). A thrown
# parse_error unwinding while a live GPU device had hooked SEH dispatch crashed the process (F15).
target_compile_definitions(crd-material-cook PRIVATE TOML_EXCEPTIONS=0)

crd_enable_default_pch(crd-material-cook)
