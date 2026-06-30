#pragma once

// crd-hesap-interp — umbrella (Phase 3.1.6 v13; ADR-0095). Interpolation: 1-D (piecewise/spline/polynomial/rational)
// + scattered & gridded N-D. The certification-grade data-fitting layer (determinism-by-construction + allocation-free
// streaming + error-tier-exposing). v13-a ships the 1-D piecewise substrate + the safety contract.

#include <crd/hesap/interp/akima.hpp>
#include <crd/hesap/interp/barycentric.hpp>
#include <crd/hesap/interp/clough_tocher.hpp>
#include <crd/hesap/interp/cubic_spline.hpp>
#include <crd/hesap/interp/grid.hpp>
#include <crd/hesap/interp/kriging.hpp>
#include <crd/hesap/interp/piecewise.hpp>
#include <crd/hesap/interp/rational.hpp>
#include <crd/hesap/interp/rbf.hpp>
#include <crd/hesap/interp/spectral.hpp>
