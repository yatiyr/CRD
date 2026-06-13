// fft.cpp — Phase 3.1.6 v10-a: translation unit for crd-hesap-fft. The FFT is header-only templates
// (fft.hpp); this TU exists so the static library has an object (and anchors future non-template helpers +
// the v10-z CLI registration block). Including the header here also compile-checks it in isolation.

#include <crd/hesap/fft/fft.hpp>

namespace crd::hesap::fft
{
// Force-instantiate the f32/f64 plans so the library carries their code (and a fresh TU catches header
// regressions even when no consumer instantiates them this build).
template class FftPlan<crd::f32>;
template class FftPlan<crd::f64>;
} // namespace crd::hesap::fft
