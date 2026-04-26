crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-log STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-log
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-log
    PUBLIC
        crd-core
        # crd-containers is needed for:
        #   1. RingBufferSink internal storage (Array<StoredLogRecord>)
        #   2. The g_log_containers channel definition lives here
        #      (see src/log_channels_first_party.cpp)
        # The reverse arrow (crd-containers -> crd-log) only carries the
        # channel-decl macro headers and does not introduce a real link
        # dependency, so MSVC's static-lib resolution handles the pair.
        crd-containers
)

target_compile_features(crd-log PUBLIC cxx_std_20)
