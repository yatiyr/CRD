add_executable(crd-render-graph-tests
    test_frame_graph.cpp  # RAF-7 Gate 7
)

target_link_libraries(crd-render-graph-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-render-asset-core
    crd-render-pass
    crd-gpu-context
    crd-render-graph
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-render-graph-tests)

catch_discover_tests(crd-render-graph-tests)

# RAF-7 live-GPU wiring: the frame graph executing on a real device via the command encoder (both backends).
add_executable(crd-render-graph-gpu-tests
    test_frame_graph_gpu.cpp
)

target_link_libraries(crd-render-graph-gpu-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-render-asset-core
    crd-render-pass
    crd-gpu-context
    crd-gpu-context-vulkan
    crd-render-graph
    Catch2::Catch2WithMain
)

target_include_directories(crd-render-graph-gpu-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-shared")

crd_enable_default_pch(crd-render-graph-gpu-tests)

if(WIN32)
    target_link_libraries(crd-render-graph-gpu-tests PRIVATE crd-gpu-context-dx12)
endif()

catch_discover_tests(crd-render-graph-gpu-tests PROPERTIES RESOURCE_LOCK crd_gpu_device)
