# crd-hesap-wavelet — Phase 3.1.6 v11 (DSP cluster, wavelets). ADR-0093.
# Plan: docs/phases/phase-3.1.6-hesap.md (v11 DETAILED PLAN, v11w-a…e); memory project_v11_dsp_plan.
# Gold standard: PyWavelets (pywt) + MATLAB Wavelet Toolbox.
#
# Own module (module-isolation cornerstone — a DAW links crd-hesap-dsp, a sci app links wavelets).
# Coefficient tables are GENERATED from pywt (scripts/gen_wavelet_coeffs.py → detail/wavelet_coeffs.hpp,
# the FFT-codelet / SUNDIALS-tableau pattern) ⇒ the engine matches pywt to machine precision.
#
# ⭐ Honest gate (ADR-0093): families/filter-banks = self-contained (QMF/orthogonality/PR) + coeffs vs pywt;
# DWT/IDWT APPLICATION = per-mode coefficients vs pywt + perfect reconstruction + run-twice determinism moat.
#
# Edges (acyclic):
#   crd-core / crd-containers / crd-memory  — types, Array/Span, IAllocator
#   crd-math    — crd::math::simd (kernels)
#   crd-hesap   — Complex<T> (CWT analytic wavelets, lower-layer raw per ADR-0078)
#   crd-hesap-fft    — FFT-based CWT / fast DWT convolution (lands at the CWT slice)

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-hesap-wavelet STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-hesap-wavelet
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-hesap-wavelet
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-math
        crd-hesap
        crd-hesap-fft
        crd-jobs   # v11w-d: multi-threaded batched CWT (independent scales — per-job plans, bit-identical)
    PRIVATE
        crd-warnings
)

target_compile_features(crd-hesap-wavelet PUBLIC cxx_std_20)
crd_enable_default_pch(crd-hesap-wavelet)
