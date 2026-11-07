# crd-hesap-net -- the network and media math of HESAP-NET: packet erasure codes (RFC 5510 Reed-Solomon, RFC 8627
# FlexFEC, RFC 2198 RED, interleavers) with the dispatched GF(2^8) kernels, the RTT, jitter, delay-trend and rate
# estimators and the clock offset and drift filters (ADR-0065 §29 amendment of 2026-11-07).
#
# Its one rule is its closure: crd-net links it, so it stays on crd-core, crd-containers and crd-math and reaches no
# job system.
# hesap-comms, hesap-stats and hesap-interp all reach crd-jobs (through hesap-dsp, hesap-fft, hesap-dense and
# hesap-special), which has no ARM64 fiber context before JOBS-PORT.2, and a network client links no FFT.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-net STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-net
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-net
    PUBLIC
        crd-core
        crd-containers
    PRIVATE
        crd-math
        crd-warnings
)

target_compile_features(crd-hesap-net PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-net)

# The GF(2^8) dot product and addition, one body per CPU level (ADR-0134); src/erasure_kernel_dispatch.cpp lists them.
crd_add_cpu_kernels(crd-hesap-net NAME erasure LEVELS portable sse2 avx2 avx512 neon VARIANTS avx2+gfni avx512+gfni
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/erasure_kernels.cpp")
