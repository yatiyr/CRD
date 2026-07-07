# crd-hesap-autodiff — Phase 3.1.6 v15 (forward mode) + v16 (reverse mode). The engine's DIFFERENTIATION layer.
# ADR-0097. One module, forward + reverse; header-only sub-headers with (later slices) PRIVATE/TU-local heavy deps
# so a scalar-Dual-only consumer drags neither the tensor JVPs nor the solver bridges (the link-isolation smoke).
#
# Edges (acyclic; ADR-0097 §5 — autodiff is LOWER than the solvers):
#   crd-core / crd-containers / crd-memory — types, Span, IAllocator (the substrate)
#   crd-math                               — the crd::math cmath surface the chain rule calls (PUBLIC: on the API)
#   Later slices add PUBLIC crd-jobs (v15-d SIMD/parallel drivers, the {1..16} moat) and PRIVATE/include-only edges
#   to hesap-{tensor,dense,fft,dsp,interp,special} for the suite JVPs (v15-e/f) — NEVER a link edge into a lean
#   scalar consumer; gated by the link-isolation smoke. The new acyclic edge hesap-opt -> hesap-autodiff (v15-a,
#   the Dual migration) makes opt the consumer; opt re-exports Dual so its suite compiles unchanged (zero regressions).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-autodiff STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-autodiff
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-autodiff
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math       # crd::math cmath surface — the chain-rule transcendental cores (header-only, deterministic)
        crd-jobs       # v16-a: data-parallel batched gradient (deterministic fixed-order reduce — the {1..16} moat)
        crd-hesap      # v15-z: the CLI command registry (hesap.ad.* registration). crd-hesap depends only on
                       # core/math (NOT autodiff), so this edge is acyclic — autodiff stays below the solvers.
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-autodiff PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-autodiff)
