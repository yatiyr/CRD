# crd-hesap-sched — Phase 3.1.6 v0d.
#
# Task-DAG / tile-scheduling substrate over crd::jobs. Used by BLAS L3
# (gemm / syrk / herk / syr2k / her2k / trmm / trsm) to dispatch
# independent tile computations across worker fibers without coding the
# parallelism inline at every call site.
#
# v0d foundation surface (this slice):
#   - TileId / TileAccess / TileDep value types
#   - TaskGraph that batches independent tasks and submits them via
#     `crd::jobs::parallel_for` (no formal dep-tracking DAG yet —
#     filed as `v0d-formal-dag` follow-on).
#   - `parallel_tiles_for` convenience that handles tile-index → linear
#     mapping for 2D tile grids.
#
# Future (`v0d-formal-dag` follow-on): real PLASMA / PaRSEC-style DAG
# with topological sort + work-stealing across dependent tasks.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-sched STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-sched
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-sched
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-jobs
        crd-hesap
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-sched PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-sched)
