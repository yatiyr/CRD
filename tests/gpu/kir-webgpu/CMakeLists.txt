# crd-kir-webgpu tests — Phase 3.1.6 v17-d. Guarded on the backend target existing (wgpu-native present).
if(NOT TARGET crd-kir-webgpu)
    return()
endif()

add_executable(crd-kir-webgpu-tests
    test_backend_webgpu.cpp
)

target_link_libraries(crd-kir-webgpu-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-webgpu
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-webgpu-tests)

# wgpu-native ships as a DLL; copy it next to the exe so the test can start.
add_custom_command(TARGET crd-kir-webgpu-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CRD_WGPU_DLL}" "$<TARGET_FILE_DIR:crd-kir-webgpu-tests>"
)

catch_discover_tests(crd-kir-webgpu-tests DISCOVERY_MODE PRE_TEST)
