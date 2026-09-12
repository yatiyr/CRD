# crd-kir-metal — Phase 3.1.6 v17-d: the CKIR Metal backend. `KirBackendMetal` over Apple Metal (Objective-C++). A
# SEPARATE module (ADR-0096 lean-consumer). APPLE-ONLY — skipped elsewhere so Windows/Linux build unaffected. No extra
# install (Metal + Foundation ship with macOS). Built + validated on real Apple silicon at Part C. ADR-0098.

if(NOT APPLE)
    message(STATUS "crd-kir-metal: not macOS -- skipping the Metal backend (built + validated at Part C).")
    return()
endif()

enable_language(OBJCXX)

crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir-metal STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/backend_metal.mm"
    ${HEADERS}
)

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/backend_metal.mm" PROPERTIES
    COMPILE_FLAGS "-fobjc-arc" # ARC: the id<MTL...> handles auto-release
)

target_include_directories(crd-kir-metal
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)

target_link_libraries(crd-kir-metal
    PUBLIC
        crd-kir
        crd-core
        crd-containers
        crd-memory
        "${METAL_FRAMEWORK}"
        "${FOUNDATION_FRAMEWORK}"
    PRIVATE
        crd-warnings
)

target_compile_features(crd-kir-metal PUBLIC cxx_std_20)
