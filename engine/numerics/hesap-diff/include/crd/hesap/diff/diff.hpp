#pragma once

// crd-hesap-diff umbrella — Phase 3.1.6 v13-l/m (ADR-0095). Numerical differentiation:
//   finite_difference.hpp — Fornberg arbitrary-stencil weights + central/Ridders + FD gradient (v13-l)
//   complex_step.hpp      — ★★ machine-exact derivative/gradient/Jacobian (v13-m)
//   savitzky_golay.hpp    — noise-robust polynomial-fit differentiation (v13-m)
//   spectral.hpp          — Chebyshev/Fourier differentiation matrices (v13-m)

#include <crd/hesap/diff/complex_step.hpp>
#include <crd/hesap/diff/finite_difference.hpp>
#include <crd/hesap/diff/savitzky_golay.hpp>
#include <crd/hesap/diff/spectral.hpp>
