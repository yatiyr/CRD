# crd-hesap-dsp — Phase 3.1.6 v11 (DSP cluster, core + adaptive). ADR-0093.
# Plan: docs/phases/phase-3.1.6-hesap.md (the v11 DETAILED PLAN); memory project_v11_dsp_plan.
# Gold standards: scipy.signal + MATLAB Signal Processing Toolbox + Intel IPP + liquid-dsp + CMSIS-DSP.
#
# ⭐ The honest gate: filter DESIGN gates on SPEC-COMPLIANCE (+ coeffs-to-N-digits), NOT bit-match; filter
# APPLICATION gates BIT-EXACT + the {1..16} determinism moat (streaming only). Two-layer (ADR-0078): typed
# whole-array batch API upper + allocation-free stateful streaming kernels lower (the DAW/SDR hot loop).
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory  — types, Array/Span, IAllocator
#   crd-math    — crd::math::simd (the streaming/kernel SIMD)
#   crd-units   — Frequency<T> + the v11-a DecibelRatio/DecibelPower + NormalizedFrequency
#   crd-hesap   — Complex<T> (filters are complex-capable; lower-layer raw per ADR-0078)
#   crd-hesap-fft    — spectral / fast-convolution / Hilbert / CZT (lands at the spectral slices)
#   crd-hesap-eigen  — DPSS multitaper + MUSIC/ESPRIT subspace + AR (lands at the estimator slices)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-dsp STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-dsp
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-dsp
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-units
        crd-hesap
        crd-hesap-fft
        crd-hesap-eigen
        crd-jobs   # v11-m: parallel batched FFT (Welch/STFT — many independent FFTs, per-job plans)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-dsp PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-dsp)
