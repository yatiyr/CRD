# crd-chir — CEIR-32 (CHIR: the Cerid high-level language layer).
#
# CHIR is the SOURCE-language layer (ADR-0108/0109): modules, generics, ADTs, closures, ownership, async/state/events —
# it LOWERS/ERASES into CEIR (one-way). This module is a host-only compiler frontend (ADR-0128 D6): the structured
# SOURCE MODEL (semantic nodes + stable ids + source spans + a layout side-table, ADR-0128 D5) + its projections. Text
# and the CR-D007 graph are two projections of the ONE model; the graph is a SCHEMA (not an editor), lowered through the
# SAME CHIR->CEIR path (§180 #13). CEIR-33/D7E renders the schema.
#
# ⛔ Deps, and ONLY these (ADR-0128 D6 / ADR-0109 §4): crd-ceir (the StableId/SourceLoc identity types + the lowering
# target) + crd-core/log/memory/containers/units. One-way -> crd-ceir; never sideways/up. Acyclic by construction.
#
# CEIR-32b: the source model (node.hpp/cpp) + the graph-schema projection (schema.hpp/cpp). CEIR-32c: the CHIR TEXT
# projection (text.hpp/cpp — print_chir/parse_chir). ~~NO text parser (32c)~~ (superseded 32c). NO CHIR->CEIR lowering
# (32d) yet.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-chir STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-chir
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-chir
    PUBLIC
        crd-ceir         # ADR-0128 D5/D6: the shared StableId/SourceLoc identity types + the one-way lowering target
        crd-core         # types
        crd-log
        crd-memory       # IAllocator
        crd-containers   # Array / String / StringView / fnv1a_64 (the stable-id derivation)
        crd-units
)

target_compile_features(crd-chir PUBLIC cxx_std_20)
crd_enable_default_pch(crd-chir)
