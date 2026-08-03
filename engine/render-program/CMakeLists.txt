# crd-render-program — RAF-4 (D-007 "RAF band"; mission §4.1/4.2/§9).
#
# The shader + program CONTRACT: stages · typed stage I/O · resource declarations (kind + frequency) · the resolved
# deterministic binding LAYOUT · the interface signature hash (interface-change invalidation) · the variant-key axes.
# IR-agnostic + backend-agnostic — a cooker maps a CKIR KEntry into a ShaderModuleContract; this module validates and
# hashes with no device. Built on render-asset-core (shared binding vocab + InterfaceHash + diagnostics).
# Leaf-ish module — deps: core / containers / memory / render-asset-core.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-render-program STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-render-program
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-render-program
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-render-asset-core   # shared BindingFrequency/BindingKind · InterfaceHash · Diagnostic
)

target_compile_features(crd-render-program PUBLIC cxx_std_20)
crd_enable_default_pch(crd-render-program)
