// The engine's canned scalar functions f: R^n → R (the hesap.ad commands' `func`), generic over the carrier: f64,
// JetPackD and HyperDual. They are polynomials, so every carrier differentiates them exactly. They live in the level
// namespace (ADR-0134 §5) because both general code and the autodiff kernel units instantiate them.

#pragma once

#include <crd/core/kernel_level.hpp>
#include <crd/hesap/autodiff/autodiff_kernels.hpp>

namespace crd::hesap::autodiff::inline CRD_MATH_LEVEL_NS::canned
{

// Σ 100 (x_{i+1} - x_i²)² + (1 - x_i)².
struct Rosenbrock
{
    template <class T> T operator()(const T* x, int n) const
    {
        T s = T(0.0);
        for (int i = 0; i < n - 1; ++i)
        {
            const T a = x[i + 1] - x[i] * x[i];
            const T b = T(1.0) - x[i];
            s = s + T(100.0) * a * a + b * b;
        }
        return s;
    }
};

// Σ x_i².
struct Sphere
{
    template <class T> T operator()(const T* x, int n) const
    {
        T s = T(0.0);
        for (int i = 0; i < n; ++i)
        {
            s = s + x[i] * x[i];
        }
        return s;
    }
};

// Σ x_i³.
struct Cubes
{
    template <class T> T operator()(const T* x, int n) const
    {
        T s = T(0.0);
        for (int i = 0; i < n; ++i)
        {
            s = s + x[i] * x[i] * x[i];
        }
        return s;
    }
};

// fn(functor) with the functor of `which`.
template <class Fn> void visit(CannedFunction which, const Fn& fn)
{
    switch (which)
    {
        case CannedFunction::Sphere:
            fn(Sphere{});
            return;
        case CannedFunction::Cubes:
            fn(Cubes{});
            return;
        case CannedFunction::Rosenbrock:
            break;
    }
    fn(Rosenbrock{});
}

} // namespace crd::hesap::autodiff::inline CRD_MATH_LEVEL_NS::canned
