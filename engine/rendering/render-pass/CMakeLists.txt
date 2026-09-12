# crd-render-pass — RAF-6 (D-007 "RAF band"; mission §8).
#
# The PASS-EXECUTOR REGISTRY that retires the central FramePassKind enum + frame_runtime switch: passes name a
# REGISTERED executor by a stable ExecutorTypeId (resolved once at cook, never a string at record); each executor has
# a typed, versioned SCHEMA (params · resource slots · queue); a pass carries a typed PAYLOAD (no void*) validated
# against the schema. Engine built-ins register the 8; apps register their own without editing an engine enum.
# Backend-/IR-agnostic; validated with no device. Leaf-ish module — deps: core / containers / memory / render-asset-core.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-render-pass STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-render-pass
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-render-pass
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-render-asset-core   # structured Diagnostic + hashing
)

target_compile_features(crd-render-pass PUBLIC cxx_std_20)
crd_enable_default_pch(crd-render-pass)
