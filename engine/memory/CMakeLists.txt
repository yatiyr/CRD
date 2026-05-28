crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-memory STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-memory
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-memory
    PUBLIC
        crd-core
        crd-log
        crd-vm   # ADR-0085 S2: VirtualMemoryAllocator's page source (crd::vm reserve/commit)
)

target_compile_features(crd-memory PUBLIC cxx_std_20)
crd_enable_default_pch(crd-memory)
