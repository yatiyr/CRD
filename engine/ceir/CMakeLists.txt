# crd-ceir — CEIR (Cerid Execution IR). ADR-0109. The host-only execution/orchestration IR + the master spine of
# D-007 (mission §2). CEIR-1a = the core IR structures (Context / Module / Operation / Value / Block / Region) with
# intrusive, in-arena def-use (RAUW = splice, zero-alloc) and O(1) op/block edit. Host-only + device-free-testable —
# CEIR-1..9 land with no GPU contact.
#
# ⛔ Edges (acyclic, ADR-0109 §4.1 — invariant I5): crd-core / crd-log / crd-memory / crd-containers / crd-units
# ONLY. NO gpu-context, NO kir, NO render-graph, NO jobs — those are reached through the dependency-inversion
# bridges (crd-ceir-host / crd-ceir-gpu), never a link edge into the core. The I5 guard test enforces this.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-ceir STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-ceir
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-ceir
    PUBLIC
        crd-core
        crd-log
        crd-memory
        crd-containers
        crd-units
    PRIVATE
        crd-warnings
)

target_compile_features(crd-ceir PUBLIC cxx_std_20)
crd_enable_default_pch(crd-ceir)
