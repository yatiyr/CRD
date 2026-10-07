add_executable(crd-perf-gpu-bridge-tests
    test_frame_graph_gpu_backend.cpp  # DIAG.6b(i): IFrameGraph timings -> resolved crd-perf GPU spans (fake-verified)
    test_gpu_resources_diag.cpp       # the gpu.resources diagnostic command over fake contexts and frame graphs
)

target_link_libraries(crd-perf-gpu-bridge-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-time
    crd-gpu-context
    crd-perf
    crd-perf-gpu-bridge
    crd-asset-io      # the JSON reader that parses the diagnostic responses
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-perf-gpu-bridge-tests)

crd_discover_tests(crd-perf-gpu-bridge-tests)
