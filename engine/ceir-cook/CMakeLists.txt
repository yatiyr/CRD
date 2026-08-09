# crd-ceir-cook — the CEIR asset COOK bridge (D-007 CEIR-7a; ADR-0109 §4.2). Cooks a crd::ceir::Module into a CRDR
# container (the render-asset CookedHeader in a 'META' chunk, the CEIR binary program in a 'CEIR' chunk, and the §106
# dependency record in a 'CDEP' chunk), keyed by the CONTENT hash. ⛔ A BRIDGE MODULE: it depends on crd-ceir AND the
# ASSET layer (crd-resources' CRDR container + crd-render-asset-core's CookedHeader envelope). crd-ceir itself NEVER
# link-edges to either — the crd-ceir-invariants gate (scripts/check_ceir_invariants) keeps the core asset-free.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-ceir-cook STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-ceir-cook
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-ceir-cook
    PUBLIC
        crd-ceir      # the IR + interface_hash / stable_hash / collect_dependencies + the binary serializer
        crd-core      # types, asserts
        crd-containers
        crd-memory
    PRIVATE
        crd-resources         # the CRDR container writer/reader — the 'CEIR' chunk (NOT in any public header)
        crd-render-asset-core # the CookedHeader cooked-envelope + interface/content-hash vocabulary (impl only)
        crd-warnings
)

target_compile_features(crd-ceir-cook PUBLIC cxx_std_20)
crd_enable_default_pch(crd-ceir-cook)
