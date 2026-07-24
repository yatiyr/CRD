crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-anim STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-anim
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-anim
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-hesap-interp   # GEO-8: the ONE keyframe/curve engine (shared with GEO-9 timelines + GEO-10 audio)
        crd-resources
)

target_compile_features(crd-anim PUBLIC cxx_std_20)

crd_enable_default_pch(crd-anim)
