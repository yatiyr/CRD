add_executable(crd-gpu-context-tests
    test_command_model.cpp  # RAF-2a: canonical command model (device-free structural gate)
)

target_link_libraries(crd-gpu-context-tests PRIVATE
    crd-core
    crd-containers
    crd-gpu-context
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-gpu-context-tests)

catch_discover_tests(crd-gpu-context-tests)

# RAF-2b/2c: the canonical command ENCODER on real GPUs (both backends) + cross-backend + legacy-verb parity.
# Separate target from the device-free structural gate above (this one links the backends + kir + gpu-shared).
add_executable(crd-gpu-context-encoder-gpu-tests
    test_command_encoder_gpu.cpp
)

target_link_libraries(crd-gpu-context-encoder-gpu-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-gpu-context
    crd-gpu-context-vulkan
    Catch2::Catch2WithMain
)

# The shared, backend-neutral CKIR raster triangle (the identical graph both backends draw).
target_include_directories(crd-gpu-context-encoder-gpu-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../gpu-shared")

crd_enable_default_pch(crd-gpu-context-encoder-gpu-tests)

if(WIN32) # the DX12 backend exists only on Windows
    target_link_libraries(crd-gpu-context-encoder-gpu-tests PRIVATE crd-gpu-context-dx12)
endif()

# GPU-device tests share a RESOURCE_LOCK so the sweep never runs two real devices concurrently (driver contention).
catch_discover_tests(crd-gpu-context-encoder-gpu-tests PROPERTIES RESOURCE_LOCK crd_gpu_device)
