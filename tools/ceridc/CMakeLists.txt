# ceridc — GEO-11 (D-007 row 76): the AGENT SURFACE — every GEO operation as a headless verb with a JSON
# report, over TWO transports: the CLI and an MCP (JSON-RPC 2.0 stdio) server. The verbs live in a LIBRARY
# (crd-ceridc) so the agent gate drives the REAL protocol handler in-process; the binary is the thin shell.

crd_collect_sources(CERIDC_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
list(FILTER CERIDC_SOURCES EXCLUDE REGEX "main\\.cpp$")
crd_collect_sources(CERIDC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-ceridc STATIC ${CERIDC_SOURCES} ${CERIDC_HEADERS})

target_include_directories(crd-ceridc
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-ceridc
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-platform
        crd-asset-io
        crd-resources
        crd-scene
        crd-time
        crd-timeline
        crd-audio
        crd-cooker
)

target_compile_features(crd-ceridc PUBLIC cxx_std_20)
crd_enable_default_pch(crd-ceridc)

add_executable(ceridc "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(ceridc PRIVATE crd-ceridc)
crd_enable_default_pch(ceridc)
