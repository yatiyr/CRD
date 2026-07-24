# crd-timeline — GEO-9 (D-007 row 74): the TIMELINE/SEQUENCE resource — OTIO-shaped editorial data
# (tracks/clips/transitions/markers/time-effects/automation) in RATIONAL time (crd-time), referencing
# media by ResourceId/URI (never embedding), evaluated deterministically, rendered to EXR sequences
# (crd-resources' own codec). The `.otio` interchange edge lives in crd-asset-io; the cook handler in
# tools/asset_cooker. hesap-interp is the ONE curve engine (automation shares it with GEO-8 anim).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-timeline STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-timeline
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-timeline
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-time
        crd-resources
        crd-hesap-interp
)

target_compile_features(crd-timeline PUBLIC cxx_std_20)
crd_enable_default_pch(crd-timeline)
