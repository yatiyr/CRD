# crd-perf-gpu-bridge — DIAG.6b(i): the adapter that turns an IFrameGraph's per-pass GPU timings into resolved
# crd-perf GPU spans.
#
# It exists as its OWN module because it is the one place that names BOTH crd-perf (the profiler substrate) and
# crd-gpu-context (the frame graph): perf DEPENDS containers core jobs memory time (NO gpu-context) and gpu-context
# DEPENDS containers core render-asset-core (NO perf), so neither can host the adapter without gaining an upward
# dependency. A leaf module that links both keeps that layering intact — no engine module depends back on the bridge.
# For the same reason it owns the gpu.resources diagnostic command (a host such as ceridc binds it).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-perf-gpu-bridge STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-perf-gpu-bridge
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-perf-gpu-bridge
    PUBLIC
        crd-containers
        crd-core
        crd-gpu-context # IFrameGraph — the per-pass GPU timings source
        crd-perf        # emit_gpu_span_on / IProfilerGpuBackend — the resolved-span sink
        crd-time        # GpuClockCalibration (the calibrated path; the bridge stays uncalibrated for now)
)

target_compile_features(crd-perf-gpu-bridge PUBLIC cxx_std_20)
crd_enable_default_pch(crd-perf-gpu-bridge)
