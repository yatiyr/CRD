# crd-hesap-tensor — Phase 3.1.6 v14 (the TENSORS cluster). The N-D tensor substrate. ADR-0096.
#
# Edges (acyclic; ADR-0096 §1 — lean-consumer via link-edge visibility):
#   crd-core / crd-containers / crd-memory — types, Span, IAllocator (PUBLIC: on the API surface)
#   Later slices add PRIVATE TU-local edges (hesap-sparse for sparse tensors, hesap-dense for
#   decompositions) — NEVER PUBLIC, gated by the link-isolation smoke. The Philox counter-RNG
#   (deterministic SR) is consumed include-only (header-only constexpr) — no link edge.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-tensor STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-tensor
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        # Include-ONLY Philox edge (ADR-0096 §1): dtypes.hpp consumes the header-only
        # constexpr counter-RNG for deterministic SR without link-dragging the
        # hesap-stats -> hesap-special stack into every tensor consumer (the
        # units->math precedent). philox.hpp includes only leaf headers.
        "${CMAKE_SOURCE_DIR}/engine/hesap-stats/include"
)

target_link_libraries(crd-hesap-tensor
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math  # float_convert.hpp — the migrated f16/bf16/FP8 format primitives (header-only)
        crd-jobs  # reduce.hpp Tier-D parallel blocks (deterministic parallel_for; acyclic)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-tensor PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-tensor)
