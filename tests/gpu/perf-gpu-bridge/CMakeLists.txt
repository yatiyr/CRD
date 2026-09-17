add_executable(crd-perf-gpu-bridge-tests
    test_frame_graph_gpu_backend.cpp  # DIAG.6b(i): IFrameGraph timings -> resolved crd-perf GPU spans (fake-verified)
)

target_link_libraries(crd-perf-gpu-bridge-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-time
    crd-gpu-context
    crd-perf
    crd-perf-gpu-bridge
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-perf-gpu-bridge-tests)

crd_discover_tests(crd-perf-gpu-bridge-tests)
