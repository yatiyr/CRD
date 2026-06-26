#pragma once

// crd-hesap-special umbrella — Phase 3.1.6 v12. SPECIAL FUNCTIONS + orthogonal polynomials: the reusable
// substrate under the statistics distribution layer (v12-h…), v13 Gauss quadrature (Golub-Welsch), and DSP.
// ADR-0094 (v12). Module-isolation cornerstone: a leaf — depends only on core/containers/memory/math + crd-hesap
// (Complex<T>), never on crd-hesap-stats.
//
//   v12-a (this batch): gamma family · incomplete gamma/beta + inverses · erf family · Dawson/Faddeeva.
//   v12-b: Bessel & Airy.   v12-c: orthogonal polynomials + Golub-Welsch.   v12-d: hypergeometric/zeta/elliptic/…

#include <crd/hesap/special/airy.hpp>
#include <crd/hesap/special/batch.hpp>
#include <crd/hesap/special/bessel.hpp>
#include <crd/hesap/special/elliptic.hpp>
#include <crd/hesap/special/erf.hpp>
#include <crd/hesap/special/expint.hpp>
#include <crd/hesap/special/fresnel.hpp>
#include <crd/hesap/special/gamma.hpp>
#include <crd/hesap/special/hypergeom.hpp>
#include <crd/hesap/special/lambertw.hpp>
#include <crd/hesap/special/incomplete.hpp>
#include <crd/hesap/special/marcum.hpp>
#include <crd/hesap/special/orthopoly.hpp>
#include <crd/hesap/special/struve.hpp>
#include <crd/hesap/special/zeta.hpp>
