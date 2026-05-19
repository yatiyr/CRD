# crd-hesap — Phase 3.1.6 v0a.
#
# Numerical-substrate root module: shared value types + abstractions used
# by every hesap sibling (hesap-dense, future hesap-sparse, hesap-sched
# v0d, hesap-bench v0f, ...).
#
# v0a scope (substrate scaffolding only; no math kernels yet):
#   - Complex<T>                 — Cerid-native complex value type
#                                  (Smith 1962 robust divide, hypot abs,
#                                  atan2 arg). f32/f64 specialisations.
#   - LinearOp<T>                — matrix-free linear-operator interface.
#                                  3 pure virtuals (apply / n_rows / n_cols)
#                                  + 2 virtual-with-default (apply_transpose
#                                  / apply_adjoint). NEW VIRTUALS APPEND
#                                  AT END forever — vtable stability rule
#                                  per feedback_vtable_stability_append_at_end.
#   - MatrixId / VectorId        — opaque 64-bit handles (gen:32 | idx:32),
#                                  same shape as scene::EntityId.
#   - 'HDV0' CRDR FourCC         — pin for the v0 hesap-dense on-disk format
#                                  (filled in v0f when load/save lands).
#   - CLI protocol scaffolding   — CommandSchema / CommandResult /
#                                  CommandRegistry (Meyers singleton) +
#                                  CRD_HESAP_CLI_REGISTER_MODULE static-
#                                  init macro + minimal write-only JSON
#                                  emitter + MCP-tool-descriptor types.
#                                  No parser / REPL / RPC server — that
#                                  substrate ships with crd-cli in Phase 4.0.
#                                  Per ADR-0081 §10: every public op from
#                                  v0b forward registers its schema.
#
# Two-layer typed architecture (ADR-0078 §5): hesap is dimensionless math,
# so almost no Quantity<D,T> at this surface. Bridges enter only when a
# typed consumer (eylem in v1f-articulation onward) drives a hesap op.
#
# Determinism (ADR-0063): Complex<T> ops are scalar in v0a; SIMD bit-exact
# parity lands v0b. The algorithms chosen (Smith division, hypot, atan2)
# extend to SIMD without reordering reductions.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap
    PUBLIC
        crd-core
        crd-containers
        crd-memory
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap)
