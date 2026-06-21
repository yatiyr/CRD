#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-wavelet — wavelet cluster umbrella (Phase 3.1.6 v11w). ADR-0093.
//
// Wavelets as a standalone module (a DAW links crd-hesap-dsp; a scientific /
// medical-imaging app links the wavelet transforms). Gold standard: PyWavelets
// (+ MATLAB Wavelet Toolbox). The filter-bank coefficients are GENERATED from
// pywt (scripts/gen_wavelet_coeffs.py → detail/wavelet_coeffs.hpp) so the engine
// matches pywt to machine precision.
//
// Honest gate (ADR-0093): families/filter-banks = self-contained QMF /
// orthogonality / PR + coeffs vs pywt; DWT/IDWT = per-mode coefficients vs pywt
// + perfect reconstruction + run-twice bit-identical (the determinism moat).
// ---------------------------------------------------------------------------

#include <crd/hesap/wavelet/cwt.hpp>
#include <crd/hesap/wavelet/denoise.hpp>
#include <crd/hesap/wavelet/dwt.hpp>
#include <crd/hesap/wavelet/dwt2.hpp>
#include <crd/hesap/wavelet/families.hpp>
#include <crd/hesap/wavelet/modwt.hpp>
#include <crd/hesap/wavelet/swt.hpp>
#include <crd/hesap/wavelet/wpt.hpp>
