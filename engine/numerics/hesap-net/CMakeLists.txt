# crd-hesap-net -- the network and media math of HESAP-NET: packet erasure codes (RFC 5510 Reed-Solomon, RFC 8627
# FlexFEC, RFC 2198 RED, RFC 6330 RaptorQ, interleavers) with the dispatched GF(2^8) kernels, the RTT, jitter, delay-trend and rate
# estimators, the clock offset and drift filters, the quantisation of replicated state, its sampling between and past
# snapshots, and the loss and delay models of a simulated link (ADR-0065 §29 amendment of 2026-11-07).
#
# Its one rule is its closure: crd-net links it, so it stays on crd-core, crd-containers and crd-math and reaches no
# job system.
# hesap-comms, hesap-stats and hesap-interp all reach crd-jobs (through hesap-dsp, hesap-fft, hesap-dense and
# hesap-special), which has no ARM64 fiber context before JOBS-PORT.2, and a network client links no FFT. Of two of them
# it reads headers and links nothing (the Philox precedent, ADR-0096 §1): the Philox stream and normal sampler of
# hesap-stats, whose bulk fill it never calls, and hesap-special's erfc, a template on crd-math.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-net STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-net
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/engine/numerics/hesap-stats/include>"
    PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/numerics/hesap-special/include"
)

target_link_libraries(crd-hesap-net
    PUBLIC
        crd-core
        crd-containers
        crd-math
    PRIVATE
        $<BUILD_INTERFACE:crd-warnings>
)

target_compile_features(crd-hesap-net PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-net)

# crd-net links it privately (its congestion controllers' estimators) and no packaged public header includes its
# headers: the package carries its archive alone, link-only behind Cerid::net.
crd_package_link_only(crd-hesap-net NAME hesap-net)

# The GF(2^8) dot product and addition, one body per CPU level (ADR-0134); src/erasure_kernel_dispatch.cpp lists them.
crd_add_cpu_kernels(crd-hesap-net NAME erasure LEVELS portable sse2 avx2 avx512 neon VARIANTS avx2+gfni avx512+gfni
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/erasure_kernels.cpp")

# Bounded floats and smallest-three quaternions, one body over each CPU level's lanes; src/quantize_kernel_dispatch.cpp
# lists them.
crd_add_cpu_kernels(crd-hesap-net NAME quantize LEVELS portable sse2 avx2 avx512 neon INCLUDES crd-math
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/quantize_kernels.cpp")

# Interpolation and extrapolation of replicated state, one body over each CPU level's lanes;
# src/motion_kernel_dispatch.cpp lists them.
crd_add_cpu_kernels(crd-hesap-net NAME motion LEVELS portable sse2 avx2 avx512 neon INCLUDES crd-math
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/motion_kernels.cpp")
