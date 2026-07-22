# kir_autotune — ADR-0098 §4 · AS-2b: the offline auto-scheduler CLI (GEMM schedule search → checked-in tuning DB).
# Guarded on the CUDA backend (the search runs on the GPU via NVRTC); absent ⇒ the tool is skipped like crd-kir-cuda itself.
if(NOT TARGET crd-kir-cuda)
    return()
endif()

add_executable(kir_autotune
    src/main.cpp
)

target_link_libraries(kir_autotune PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-kir-cuda
)

target_compile_definitions(kir_autotune PRIVATE _CRT_SECURE_NO_WARNINGS) # std::fopen for the generated-header write (offline CLI)
target_compile_features(kir_autotune PRIVATE cxx_std_20)
crd_enable_default_pch(kir_autotune)
