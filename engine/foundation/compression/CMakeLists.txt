# crd-compression -- the lossless compression codecs (OWN.1, capability compression.codecs; decision 1 of
# docs/design/capability-ownership.md): compression belongs to a foundation module, so every module that reads or
# writes compressed bytes reaches one owner below it.
#
# Ships the engine's own raw DEFLATE and inflate with the zlib wrapper and Adler-32 (deflate.hpp, zero third-party),
# the binding of the pinned zstd (zstd.hpp), whose header stays private to this module, and the ZIP container over
# them (zip.hpp, capability compression.zip-archives: 3MF and the cooker's OPC packages, NumPy's npz). SHIP.2's crdr
# v2 chunk codecs (zstd with trained dictionaries, the GDeflate CPU paths) land here.

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

# Cerid::compression of the relocatable package (OWN.1: crd-perf's bundle sections compress through it), with the
# pinned zstd archive exported link-only beside it (Cerid::zstd): its header is private to this module, so the package
# installs no zstd header and a consumer reaches zstd only through zstd.hpp.
crd_package_library(crd-compression NAME compression INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
crd_package_dependency(libzstd_static NAME zstd)
