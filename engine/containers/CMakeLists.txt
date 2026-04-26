crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-containers STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-containers
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-containers
    PUBLIC
        crd-core
        crd-memory
        # NOTE: NO crd-log link here. The umbrella header references
        # <crd/log/log_channel.hpp> for the CRD_DECLARE_LOG_CHANNEL macro
        # but that's a header-only reference (extern declaration only).
        # The actual symbol `g_log_containers` is defined inside crd-log's
        # log_channels_first_party.cpp, so consumers who link crd-log get
        # it for free. This is the v1d cycle break: arrow goes
        # crd-log -> crd-containers, never the reverse.
)

# Header-only dependency on crd-log: the include path is needed at compile
# time so <crd/log/log_channel.hpp> resolves. No link dependency. We
# hardcode the path because crd-log may not be a target yet at this point
# in the CMake configure order. (core/build_config.hpp's generated path
# arrives via the transitive crd-core link.)
target_include_directories(crd-containers PUBLIC
    "${CMAKE_SOURCE_DIR}/engine/log/include"
)

target_compile_features(crd-containers PUBLIC cxx_std_20)
crd_enable_default_pch(crd-containers)
