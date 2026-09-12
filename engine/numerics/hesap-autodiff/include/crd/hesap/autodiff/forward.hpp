#pragma once

// forward.hpp — Phase 3.1.6 v15-a: the crd-hesap-autodiff FORWARD-mode umbrella. ADR-0097.
//
// crd-hesap-autodiff is the engine's differentiation layer (forward mode = v15, reverse mode = v16, one module).
// It is LOWER than the solvers: it depends on {core, containers, memory, math, …} to provide THEIR derivative
// rules, and opt/ode consume IT for exact gradients (the edge is always solver→autodiff — ADR-0097 §5). Enzyme is
// OUT (LLVM-plugin, MSVC-incompatible); our forward AD is operator-overloading Dual/Jet, portable to every
// toolchain incl. WASM (§6). Namespaces: crd::hesap::autodiff::forward (this) and ::reverse (v16).
//
// Slice surface (forward): this brings in the v15-a substrate. Later slices add sibling headers
// (rules_forward / hyperdual / drivers / sparsity / matrix_calculus / taylor / wirtinger — see
// docs/phases/phase-3.1.6-v15.md and the impl reference docs/research/2026-07-06-v15-forward-ad-crush.md).

#include <crd/hesap/autodiff/drivers.hpp>
#include <crd/hesap/autodiff/dual.hpp>
#include <crd/hesap/autodiff/hyperdual.hpp>
#include <crd/hesap/autodiff/jet.hpp>
#include <crd/hesap/autodiff/jet_simd.hpp>
#include <crd/hesap/autodiff/matrix_jvp.hpp>
#include <crd/hesap/autodiff/sparse_hessian.hpp>
#include <crd/hesap/autodiff/sparse_jacobian.hpp>
#include <crd/hesap/autodiff/sparsity.hpp>
#include <crd/hesap/autodiff/sparsity_hessian.hpp>
#include <crd/hesap/autodiff/suite_jvp.hpp>
#include <crd/hesap/autodiff/complex_dual.hpp>
#include <crd/hesap/autodiff/taylor.hpp>
#include <crd/hesap/autodiff/taylor_ode.hpp>
#include <crd/hesap/autodiff/taylor_tape.hpp>

#include <crd/containers/span.hpp>

#include <concepts>

namespace crd::hesap::autodiff::forward
{

// A functor usable with forward-mode AD: callable on a span of Dual<T> returning a Dual<T>. (The real-value path
// — callable on ConstSpan<T> returning T — is the same templated operator(); not re-stated in the concept since
// the gradient drivers only need the Dual path, and the value path is checked at the real call site.) The concept
// is the framework's canonical home; crd-hesap-opt re-exports it (`using autodiff::forward::DiffFunctor`).
template <typename F, typename T>
concept DiffFunctor = requires(const F& f, crd::containers::ConstSpan<Dual<T>> xd) {
    { f(xd) } -> std::convertible_to<Dual<T>>;
};

} // namespace crd::hesap::autodiff::forward
