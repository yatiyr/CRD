# crd-compression -- the lossless compression codecs (OWN.1, capability compression.codecs; decision 1 of
# docs/design/capability-ownership.md): compression belongs to a foundation module, so every module that reads or
# writes compressed bytes reaches one owner below it.
#
# Ships the engine's own raw DEFLATE and inflate with the zlib wrapper and Adler-32 (deflate.hpp, zero third-party),
# and the binding of the pinned zstd (zstd.hpp), whose header stays private to this module. SHIP.2's crdr v2 chunk
# codecs (zstd with trained dictionaries, the GDeflate CPU paths) land here.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-compression STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-compression
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${zstd_SOURCE_DIR}/lib"
)

target_link_libraries(crd-compression
    PUBLIC
        crd-core
        crd-containers
        crd-memory
    PRIVATE
        libzstd_static
)

target_compile_features(crd-compression PUBLIC cxx_std_20)
crd_enable_default_pch(crd-compression)
