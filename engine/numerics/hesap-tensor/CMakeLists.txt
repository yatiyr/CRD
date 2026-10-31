# crd-hesap-tensor — Phase 3.1.6 v14 (the TENSORS cluster). The N-D tensor substrate. ADR-0096.
#
# Edges (acyclic; ADR-0096 §1 — lean-consumer via link-edge visibility):
#   crd-core / crd-containers / crd-memory — types, Span, IAllocator (PUBLIC: on the API surface)
#   Later slices add PRIVATE TU-local edges (hesap-sparse for sparse tensors, hesap-dense for
#   decompositions) — NEVER PUBLIC, gated by the link-isolation smoke. The Philox counter-RNG
#   (deterministic SR, sketches) is consumed include-only (header-only constexpr) — no link edge.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-tensor STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-tensor
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        # Include-ONLY Philox edge (ADR-0096 §1): dtypes.hpp (deterministic SR) and decomp.hpp (sketches) consume
        # the header-only block function (counter_rng_core.hpp), and the tensor kernels compile the block loop
        # (philox_blocks.hpp) per level, without link-dragging the hesap-stats -> hesap-special stack into every
        # tensor consumer (the units->math precedent). PhiloxRng's bulk fill runs the hesap-stats kernels, so the
        # substrate never calls it.
        "${CMAKE_SOURCE_DIR}/engine/numerics/hesap-stats/include"
        # Include-ONLY hesap-dense edge (ADR-0096 section 1): batched / einsum_exec / tt / decomp / nn / sparse_cp
        # are public header-only surfaces over the v0d GEMM, so the consumer view must see the headers; the
        # link stays with the consumer (the smoke_hesap_tensor isolation gate keeps the built lib dense-free).
        "${CMAKE_SOURCE_DIR}/engine/numerics/hesap-dense/include"
        "${CMAKE_SOURCE_DIR}/engine/numerics/hesap/include" # blas3.hpp includes crd/hesap/complex.hpp
    PRIVATE
        # Include-ONLY edges for the v14-z CLI registration TU (cli_register_tensor.cpp):
        # it consumes the CLI substrate (crd-hesap) plus the header-only einsum-exec /
        # batched / decomp / tt / nn / io surfaces (hesap-dense above, platform fs). These are
        # NOT link edges — the TU's object enters a consumer link ONLY when
        # register_tensor_cli_anchor() is referenced (the static-lib anchor pattern), and
        # such consumers link crd-hesap + crd-hesap-dense + crd-platform themselves (the
        # einsum/decomp/tt/io/nn test executables' edge set). The smoke_hesap_tensor
        # link-isolation gate (ADR-0096 §1) never references the anchor and stays intact.
        "${CMAKE_SOURCE_DIR}/engine/foundation/platform/include"
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

# SIMD-DISPATCH (ADR-0134 §5): the f32 permute regions, the Tier R binned sum, the nn quantized linears, quantizers
# and softmax and the Philox block loop (tensor_kernels.cpp), the elementwise ops, Tier D and einsum's direct products
# (tensor_ew_kernels.cpp), the batched GEMM, factorizations and solves (tensor_batched_kernels.cpp), the sparse
# TTM and MTTKRP walks (tensor_sparse_kernels.cpp) and nn's f32 pooling and convolution (tensor_nn_kernels.cpp), once
# per CPU level, and the avx2 and avx512 levels again with AVX-VNNI and AVX512-VNNI (the integer dots on dpbusd). No
# sse4.2 unit: its maddubs dots did not beat sse2's (docs/bench/2026-10-31-simd-dispatch-sse2-boards.md), so an
# sse4.2 selection runs the sse2 table.
crd_add_cpu_kernels(crd-hesap-tensor NAME tensor LEVELS portable sse2 avx2 avx512
                    VARIANTS avx2+avxvnni avx512+avx512vnni INCLUDES crd-math
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tensor_kernels.cpp"
                            "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tensor_ew_kernels.cpp"
                            "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tensor_batched_kernels.cpp"
                            "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tensor_sparse_kernels.cpp"
                            "${CMAKE_CURRENT_SOURCE_DIR}/kernels/tensor_nn_kernels.cpp")

crd_enable_default_pch(crd-hesap-tensor)
