# crd-light-cook — REN-38-E1..E6 (D-007 row 141): the LIGHTING VOCABULARY as an authored asset.
#
# ⛔⛔ `ckir_lighting.hpp` is 1100 lines of gold-standard shading — Filament punctual attenuation, Heitz LTC area
# lights, Karis split-sum IBL, PCF/PCSS/EVSM/Moment shadows, CSM, contact shadows — and the technique ABI carried
# EXACTLY ONE DIRECTIONAL LIGHT. Every one of those functions was unreachable from any asset. This module is the
# vocabulary that reaches them.
# One-way: light-cook -> {kir, containers, memory, core}; nothing depends back on it.

if(TARGET tomlplusplus::tomlplusplus)
    set(_crd_toml_target tomlplusplus::tomlplusplus)
else()
    set(_crd_toml_target tomlplusplus)
endif()

add_library(crd-light-cook
    src/lighting_asset.cpp
)

target_include_directories(crd-light-cook PUBLIC include)

target_link_libraries(crd-light-cook
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-kir
    PRIVATE
        ${_crd_toml_target}
)

target_compile_features(crd-light-cook PUBLIC cxx_std_20)
# TOML_EXCEPTIONS=0: the no-exception toml++ API (parse() returns a checked result). A thrown
# parse_error unwinding while a live GPU device had hooked SEH dispatch crashed the process (F15).
target_compile_definitions(crd-light-cook PRIVATE TOML_EXCEPTIONS=0)

crd_enable_default_pch(crd-light-cook)
