#pragma once

// crd-hesap-quadrature umbrella — Phase 3.1.6 v12-c (Part 2). Gauss quadrature (Golub-Welsch nodes/weights).
// Depends on crd-hesap-special (recurrence coeffs / Γ / B) + crd-hesap-dense (the symmetric eigensolver it REUSES).
// Deliberately NOT a leaf — that is why Golub-Welsch lives here and not in the leaf crd-hesap-special.

#include <crd/hesap/quadrature/adaptive.hpp>
#include <crd/hesap/quadrature/de.hpp>
#include <crd/hesap/quadrature/gauss.hpp>
#include <crd/hesap/quadrature/gauss_kronrod.hpp>
#include <crd/hesap/quadrature/integrate.hpp>
#include <crd/hesap/quadrature/nongauss.hpp>
#include <crd/hesap/quadrature/qags.hpp>
#include <crd/hesap/quadrature/qng.hpp>
