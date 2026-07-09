# crd-gpu-context — Phase 3.1.6 v17-i-a (ADR-0099).
#
# Backend-agnostic GPU CONTEXT foundation: IGpuContext (a live device — instance/device/queues/allocator, NO compute or
# rendering semantics) + GpuContextManager (owns the configured set). Per-backend context impls (crd-gpu-context-vulkan,
# …) create IGpuContext instances; the manager holds them. Compute (CKIR, geometry) + rendering both draw from it.
# Leaf-ish module: deps = crd-core only (it never pulls a backend's headers).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-gpu-context STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-gpu-context
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-gpu-context
    PUBLIC
        crd-core
        crd-containers   # ADR-0100: IComputeContext (span/string_view in the dispatch interface)
)

target_compile_features(crd-gpu-context PUBLIC cxx_std_20)
crd_enable_default_pch(crd-gpu-context)
