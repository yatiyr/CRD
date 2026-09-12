#pragma once

// dual.hpp — Phase 3.1.6 v15-a RE-EXPORT SHIM. Dual<T> and its forward-mode transcendentals MIGRATED to the
// canonical differentiation module crd-hesap-autodiff (ADR-0097 §2). This header keeps every `crd::hesap::opt::`
// name v7-b shipped — so every existing opt consumer (and the opt test suite) compiles UNCHANGED; zero regressions
// is the v15-a gate. New forward-mode API (Jet<T,N>, min/max/select, drivers, hyper-dual, …) lives under
// crd::hesap::autodiff::forward — do NOT widen this shim beyond the original v7-b surface.
//
// The arithmetic operators propagate via ADL on the (now aliased) autodiff::forward::Dual, so only the NAMED
// functions need re-export (a qualified `opt::sin(dual)` requires the name to exist in namespace opt).

#include <crd/hesap/autodiff/dual.hpp>

namespace crd::hesap::opt
{

using autodiff::forward::Dual;

using autodiff::forward::abs;
using autodiff::forward::cos;
using autodiff::forward::exp;
using autodiff::forward::log;
using autodiff::forward::pow;
using autodiff::forward::sin;
using autodiff::forward::sqrt;
using autodiff::forward::tan;
using autodiff::forward::tanh;

} // namespace crd::hesap::opt
