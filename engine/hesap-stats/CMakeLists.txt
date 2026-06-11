# crd-hesap-stats — Phase 3.1.6 (v12 cluster, created early as a v12-pull for v7-i). STATISTICS substrate:
# the Philox4x32-10 counter-based RNG (reproducible minibatch sampling / replay / the v12 stats cluster).
#
# Edges (acyclic; deliberately MINIMAL — the RNG is a leaf primitive):
#   crd-core        — fixed-width types + asserts
#   crd-containers  — Span (the shuffle primitive)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-stats STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-stats
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-stats
    PUBLIC
        crd-core
        crd-containers
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-stats PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-stats)
