crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-containers STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-containers
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
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
    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/foundation/log/include>"
)

target_compile_features(crd-containers PUBLIC cxx_std_20)
crd_enable_default_pch(crd-containers)

# The checksum family's kernels, one body per CPU level (HW-RESILIENCE.1, ADR-0134); src/checksum.cpp lists them. The
# CRCs: portable slicing, sse4.2's CRC32 instruction and the carry-less multiply folds, which are in no psABI level,
# and ARM64's optional CRC32 and PMULL instructions (variants; an avx2 or avx512 CPU without VPCLMULQDQ runs the sse4.2
# fold, an ARM64 CPU without them the portable table). XXH3's long loop on every vector level.
crd_add_cpu_kernels(crd-containers NAME crc LEVELS portable sse42
                    VARIANTS sse42+pclmulqdq avx2+vpclmulqdq avx512+vpclmulqdq neon+crc32 neon+pmull
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/crc_kernels.cpp")
crd_add_cpu_kernels(crd-containers NAME hash LEVELS portable sse2 avx2 avx512 neon
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/hash_kernels.cpp")

# Cerid::containers of the relocatable package; its headers name crd-log's channel macros (see above).
crd_package_library(crd-containers NAME containers INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include" INCLUDE_ONLY log)
