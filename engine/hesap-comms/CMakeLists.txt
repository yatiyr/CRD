# crd-hesap-comms — Phase 3.1.6 v11 (DSP cluster, comms / SDR). ADR-0093.
# Plan: docs/phases/phase-3.1.6-hesap.md (v11 DETAILED PLAN, v11c-a…g); memory project_v11_dsp_plan.
# Gold standard: liquid-dsp 1.6.0 (the comms C peer) + theoretical AWGN BER curves.
#
# Own module (module-isolation cornerstone — a DAW links crd-hesap-dsp, an SDR app links comms).
#
# ⭐ Honest gate (ADR-0093): modem/equalizer correctness = self-contained (Gray property + noise-free round-trip
# + BER vs the theoretical AWGN curve, the real modem spec) + constellation-set cross-check vs liquid-dsp; the
# streaming/application kernels carry the run-twice / {1..16} determinism moat. Two-layer (ADR-0078): raw Complex<T>
# kernel layer (constellations, loops); typed wrapping is the consumer's job.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory  — types, Array/Span, IAllocator
#   crd-math       — crd::math::simd
#   crd-hesap      — Complex<T>
#   crd-hesap-dsp  — FIR / RRC pulse shaping, filtering, resampling (timing recovery)
#   crd-hesap-fft  — OFDM modulation / demodulation
#   crd-hesap-stats — Philox counter-RNG + NormalSampler (DETERMINISTIC AWGN/fading — the moat)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-comms STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-comms
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-comms
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap
        crd-hesap-dsp
        crd-hesap-fft
        crd-hesap-stats
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-comms PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-comms)
