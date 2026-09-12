#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-dsp — DSP cluster umbrella (Phase 3.1.6 v11). ADR-0093.
//
// Core DSP + adaptive filters. Wavelets live in crd-hesap-wavelet, comms in
// crd-hesap-comms (module isolation — a DAW build links this, not comms).
//
// The honest gate: filter DESIGN gates on spec-compliance (+ coeffs-to-N-digits)
// vs scipy/MATLAB, NOT bit-match; filter APPLICATION gates bit-exact + the
// {1..16} determinism moat (streaming only). Two-layer (ADR-0078): typed
// whole-array batch upper + allocation-free stateful streaming kernels lower.
//
// ⭐ DATA-FLOW RULE (locked v11-a — load-bearing for every design slice):
// filters are DESIGNED in zpk and converted zpk -> sos DIRECTLY. Transfer-
// function (b/a) is an OUTPUT/interop format, NEVER a design intermediate:
// roots-of-tf (tf_to_zpk) is Wilkinson-ill-conditioned and tf coefficients
// lose precision badly above order ~8 (proven by the order-12 conditioning
// gate in test_filter.cpp). zpk_freqz evaluates the factored form directly
// (well-conditioned at any order); zpk_to_sos uses nearest-to-unit-circle
// pairing for cascade conditioning. tf_to_zpk stays as a convenience for
// USER-supplied tf, with its high-order limit documented.
// ---------------------------------------------------------------------------

#include <crd/hesap/dsp/adaptive.hpp>
#include <crd/hesap/dsp/ar.hpp>
#include <crd/hesap/dsp/ellip.hpp>
#include <crd/hesap/dsp/convolution.hpp>
#include <crd/hesap/dsp/filter.hpp>
#include <crd/hesap/dsp/filtering.hpp>
#include <crd/hesap/dsp/fir.hpp>
#include <crd/hesap/dsp/fir_special.hpp>
#include <crd/hesap/dsp/firls.hpp>
#include <crd/hesap/dsp/freqz.hpp>
#include <crd/hesap/dsp/hilbert.hpp>
#include <crd/hesap/dsp/iir.hpp>
#include <crd/hesap/dsp/iir_design.hpp>
#include <crd/hesap/dsp/lattice.hpp>
#include <crd/hesap/dsp/measurements.hpp>
#include <crd/hesap/dsp/multirate.hpp>
#include <crd/hesap/dsp/multitaper.hpp>
#include <crd/hesap/dsp/polynomial.hpp>
#include <crd/hesap/dsp/rbj.hpp>
#include <crd/hesap/dsp/remez.hpp>
#include <crd/hesap/dsp/sequences.hpp>
#include <crd/hesap/dsp/spectral.hpp>
#include <crd/hesap/dsp/state_space.hpp>
#include <crd/hesap/dsp/subspace.hpp>
#include <crd/hesap/dsp/transforms.hpp>
#include <crd/hesap/dsp/waveforms.hpp>
#include <crd/hesap/dsp/windows.hpp>
