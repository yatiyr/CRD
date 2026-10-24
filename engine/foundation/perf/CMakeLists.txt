# crd-perf -- the owned profiling and runtime diagnostics service (ADR-0133): the region profiler and CPROF captures,
# diagnostic events and bundles, symbol identities, the flight recorder, fault injection and the diag commands.
# Public headers live in `include/crd/perf/`. CRD_ENABLE_PROFILING gates the profiler's recording macros (they expand
# to ((void)0) when it is off); the library itself is built in every configuration. The GPU timestamp bridge lives in
# crd-perf-gpu-bridge (engine/gpu/perf-gpu-bridge), so this module names no GPU API.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-perf STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-perf
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-perf
    PUBLIC
        crd-core
        crd-time
        crd-jobs        # v0c jobs adapter: profiler subscribes to crd::jobs::JobObserver (substrate observes substrate, no reverse edge from jobs)
        crd-memory      # v0e allocator registry: profiler reads IAllocator::stats() snapshots into the FrameRecord ring
        crd-containers  # v0f CPROF capture: Array<u8> for buffer storage, ConstSpan<u8> for reader
        crd-toml        # DIAG.9b authored diagnostic assets: the flight policy and fault plans are TOML documents
)

target_compile_features(crd-perf PUBLIC cxx_std_20)
crd_enable_default_pch(crd-perf)

# Cerid::perf of the relocatable package (DIAG.11a: the diagnostic host's family), with the authored diagnostic assets
# its host reads at run time (assets/diag: diagnostic-host profiles, flight policies, fault plans) under
# <prefix>/share/cerid/diag, found through Cerid_ASSET_DIR.
crd_package_library(crd-perf NAME perf INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
crd_package_assets("${CMAKE_SOURCE_DIR}/assets/diag" DESTINATION diag)
