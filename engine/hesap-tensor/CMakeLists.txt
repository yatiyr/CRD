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
    PRIVATE
        # Include-ONLY edges for the v14-z CLI registration TU (cli_register_tensor.cpp):
        # it consumes the CLI substrate (crd-hesap) plus the header-only einsum-exec /
        # batched / decomp / tt / nn / io surfaces (hesap-dense, platform fs). These are
        # NOT link edges — the TU's object enters a consumer link ONLY when
        # register_tensor_cli_anchor() is referenced (the static-lib anchor pattern), and
        # such consumers link crd-hesap + crd-hesap-dense + crd-platform themselves (the
        # einsum/decomp/tt/io/nn test executables' edge set). The smoke_hesap_tensor
        # link-isolation gate (ADR-0096 §1) never references the anchor and stays intact.
        "${CMAKE_SOURCE_DIR}/engine/hesap/include"
        "${CMAKE_SOURCE_DIR}/engine/hesap-dense/include"
        "${CMAKE_SOURCE_DIR}/engine/platform/include"
)

target_link_libraries(crd-hesap-tensor
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math  # float_convert.hpp — the migrated f16/bf16/FP8 format primitives (header-only)
        crd-jobs  # reduce.hpp Tier-D parallel blocks (deterministic parallel_for; acyclic)
        crd-platform  # CI-1: io.hpp uses crd::platform::fs — the dep was HIDDEN (consumers linked it by luck;
                      #  GCC LTO static-archive ordering exposed it as undefined refs on Linux)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-tensor PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-tensor)
