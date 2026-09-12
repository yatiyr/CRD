# crd-geometry-shader-helpers — Phase 3.1.7 v0e (skeleton).
#
# The 11th crd-geometry sub-module (ADR-0076 §1): a cooker-emitted GLSL/HLSL
# library that mirrors the C++ primitive / iq-formulary functions for GPU SDF
# evaluation (renderer DFAO/DF-soft-shadow in Phase 3.5+, font MTSDF, editor
# preview). v0e ships only the skeleton — the formula-IR seed + the module
# target; the actual GLSL/HLSL emission + the ULP-conformance cooker land in
# v9e. Header-only-plus-anchor today; `shader_helpers.cpp` is the force-link TU.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-geometry-shader-helpers STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-geometry-shader-helpers
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-geometry-shader-helpers
    PUBLIC
        crd-core
        crd-containers           # Array / Span (formula-IR's flat storage)
        crd-memory               # IAllocator
        crd-math                 # Vec3<T> for the evaluator
        crd-geometry-primitives  # the C++ side of the primitive / formulary library it will mirror to GPU
    PRIVATE
        crd-platform             # filesystem (cooker writes .glsl/.hlsl to disk)
)

target_compile_features(crd-geometry-shader-helpers PUBLIC cxx_std_20)
crd_enable_default_pch(crd-geometry-shader-helpers)
