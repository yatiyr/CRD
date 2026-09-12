// autodiff.cpp — Phase 3.1.6 v15-a: translation-unit anchor for crd-hesap-autodiff.
//
// The forward-mode AD surface (dual.hpp / jet.hpp / forward.hpp) is header-only; this TU exists so the STATIC
// library has an object to compile (the crd convention — the engine uses no INTERFACE libraries). Including the
// umbrella here also compiles the headers standalone (a cheap self-contained-header check). Later slices add real
// TUs to this src/ (v15-e sparsity coloring, v15-z CLI registration).

#include <crd/hesap/autodiff/forward.hpp>
