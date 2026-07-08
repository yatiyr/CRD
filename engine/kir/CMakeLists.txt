# crd-kir — Phase 3.1.6 v17: the CERID KERNEL IR (CKIR). A NEW foundational compute substrate (a peer of crd-math /
# crd-rhi, BELOW hesap-gpu) — the two-level compute+autodiff kernel compiler (CKIR-Graph -> CKIR-Tile -> six backend
# code generators) that every Cerid consumer authors kernels on. ADR-0098.
#
# Edges (acyclic): crd-core / crd-containers / crd-memory (substrate) + crd-math (the crd::math surface — the
# transcendental primitives + the CPU reference lowering). Later slices add PUBLIC crd-rhi-compute + crd-shader
# (the Vulkan/backends), a CUDA driver layer, etc. — never a link edge back into the substrate.
# v17-a is header-only (the IR + CPU reference); the anchor source keeps the static lib linkable + hosts later codegen.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-kir
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-kir
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math       # crd::math — transcendental primitives + the deterministic CPU reference
    PRIVATE
        crd-warnings
)

target_compile_features(crd-kir PUBLIC cxx_std_20)
crd_enable_default_pch(crd-kir)
