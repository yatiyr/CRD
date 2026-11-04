# crd-platform-async -- the platform layer's asynchronous reads (ADR-0041): AsyncFile, whose completions run as crd-jobs
# jobs. Split from crd-platform on 2026-11-04 so the OS boundary links no job system (as OWN.1 split the window
# backend): crd-platform's closure, and crd-crypto's above it, then builds where crd-jobs has no fiber context yet.
# Header and namespace are unchanged; like crd-platform-window it is outside the relocatable package, whose diagnostic
# family reads no file asynchronously.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-platform-async STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-platform-async
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-platform-async
    PUBLIC
        crd-core
        crd-containers
        crd-platform
    PRIVATE
        crd-jobs
        crd-memory
)

target_compile_features(crd-platform-async PUBLIC cxx_std_20)
crd_enable_default_pch(crd-platform-async)
