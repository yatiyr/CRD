# crd-kir-webgpu — Phase 3.1.6 v17-d: the CKIR WebGPU backend (the browser/everywhere path). `KirBackendWebGpu` over
# the WebGPU C API via vendored wgpu-native (external/wgpu-native, a prebuilt native WebGPU impl). A SEPARATE module
# (ADR-0096 lean-consumer). GUARDED on the prebuilt existing ⇒ skipped where absent. ADR-0098.

set(_wgpu_dir "${CMAKE_SOURCE_DIR}/external/wgpu-native")
set(_wgpu_lib "${_wgpu_dir}/lib/wgpu_native.dll.lib")
if(NOT EXISTS "${_wgpu_dir}/include/webgpu/webgpu.h" OR NOT EXISTS "${_wgpu_lib}")
    message(STATUS "crd-kir-webgpu: wgpu-native prebuilt not found at ${_wgpu_dir} -- skipping the WebGPU backend.")
    return()
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir-webgpu STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-kir-webgpu
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${_wgpu_dir}/include"
)

target_link_libraries(crd-kir-webgpu
    PUBLIC
        crd-kir
        crd-core
        crd-containers
        crd-memory
        "${_wgpu_lib}"
    PRIVATE
        crd-warnings
)

# Make the DLL discoverable next to any consumer exe.
set(CRD_WGPU_DLL "${_wgpu_dir}/lib/wgpu_native.dll" CACHE INTERNAL "wgpu-native runtime DLL")

target_compile_features(crd-kir-webgpu PUBLIC cxx_std_20)
crd_enable_default_pch(crd-kir-webgpu)
