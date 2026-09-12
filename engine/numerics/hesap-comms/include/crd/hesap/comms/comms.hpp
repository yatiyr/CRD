#pragma once

// ---------------------------------------------------------------------------
// crd-hesap-comms — comms / SDR cluster umbrella (Phase 3.1.6 v11c). ADR-0093.
//
// Digital communications / software-defined-radio on top of crd-hesap-dsp:
// modulation, pulse shaping, timing + carrier recovery, equalization, channel
// models, OFDM. Own module (a DAW links crd-hesap-dsp, not comms).
//
// Gold standard: liquid-dsp 1.6.0 + theoretical AWGN BER curves. Honest gate:
// modem correctness = Gray property + noise-free round-trip + BER-vs-theory +
// constellation-set cross-check vs liquid; the streaming kernels carry the
// run-twice / {1..16} determinism moat. Lower-layer raw Complex<T> (ADR-0078).
// ---------------------------------------------------------------------------

#include <crd/hesap/comms/carrier.hpp>
#include <crd/hesap/comms/channel.hpp>
#include <crd/hesap/comms/equalizer.hpp>
#include <crd/hesap/comms/framing.hpp>
#include <crd/hesap/comms/loop.hpp>
#include <crd/hesap/comms/modulation.hpp>
#include <crd/hesap/comms/ofdm.hpp>
#include <crd/hesap/comms/pulse_shaping.hpp>
#include <crd/hesap/comms/timing.hpp>
