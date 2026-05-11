crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-resources STATIC ${SOURCES} ${HEADERS})

# clang-cl thin-LTO miscompiles crdr.cpp's bitcode in a way that propagates
# into the async resource-load path: the 4 load_async / load_streamed tests
# in tests/resources SEGFAULT inside crd::jobs::wait() (which is opaque to
# the LTO link, since crd-jobs has IPO=OFF for the asm fiber switch). The
# crash is a real downstream effect — bisection across all 5 source files
# in this module identifies crdr.cpp as the sole bitcode source whose
# presence triggers the miscompile (disabling LTO on any other single file
# is insufficient). NOINLINE on crdr_read alone does not clear it; the
# miscompile is somewhere in the LTO IR transforms over crdr.cpp's
# constellation of std::memcpy + Array<u8>::resize/push_back +
# ZSTD_decompress, none of which are individually problematic in MSVC LTCG
# or GCC LTO (both ship the full async test suite green at 1038/1038).
#
# Resolution: emit native code (no thin-LTO bitcode) for this one TU. MSVC
# + GCC shipping retain full LTO across the whole module; only the
# clang-cl shipping path gates this one file. Mirrors the existing
# /Od isolation on worker_pool.cpp + fiber_init.cpp in crd-jobs (which
# document a comparable single-file MSVC-LTCG compiler-bug isolation).
#
# Re-evaluate when the LLVM toolchain version is bumped (currently
# clang-cl matches the bundled VS LLVM); the bug may be fixed upstream.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
    set_source_files_properties(
        src/crdr.cpp
        PROPERTIES COMPILE_OPTIONS "-fno-lto"
    )
endif()


target_include_directories(crd-resources
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-resources
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-log
        crd-platform
    PRIVATE
        libzstd_static
        crd-jobs
)

target_include_directories(crd-resources
    PRIVATE
        "${zstd_SOURCE_DIR}/lib"
)

target_compile_features(crd-resources PUBLIC cxx_std_20)

crd_enable_default_pch(crd-resources)
