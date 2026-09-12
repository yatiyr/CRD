# crd-perf -- Detour D-003 v0a (Profiler substrate).
#
# Region-based profiler substrate. Leaf module: deps `crd-core` + `crd-time`
# only at v0a. v0c will add a callback-only edge from crd-jobs to crd-perf
# (one-way; profiler subscribes, jobs publishes). v0d (GPU) lives in
# crd-rhi-vulkan, not here.
#
# Public surface lives in `include/crd/perf/`:
#   - config.hpp        compile-time gate + sizing knobs
#   - sample.hpp        32-byte Sample POD
#   - profiler.hpp      Profiler singleton API
#   - scope.hpp         ScopedRegion + CRD_PERF_SCOPE macros
#   - perf.hpp          umbrella
#
# The substrate is gated by the existing project-wide CRD_ENABLE_PROFILING
# flag (already wired per-preset in CMakePresets.json). When the gate is
# off, every macro expands to ((void)0) and the singleton is never built.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-perf STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-perf
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-perf
    PUBLIC
        crd-core
        crd-time
        crd-jobs        # v0c jobs adapter: profiler subscribes to crd::jobs::JobObserver (substrate observes substrate, no reverse edge from jobs)
        crd-memory      # v0e allocator registry: profiler reads IAllocator::stats() snapshots into the FrameRecord ring
        crd-containers  # v0f CPROF capture: Array<u8> for buffer storage, ConstSpan<u8> for reader
)

target_compile_features(crd-perf PUBLIC cxx_std_20)
crd_enable_default_pch(crd-perf)
