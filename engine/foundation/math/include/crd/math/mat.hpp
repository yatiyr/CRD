#pragma once

// The matrix entry point: the Mat2/Mat3/Mat4 types (mat_types.hpp) plus the Mat4<f32> SIMD specializations of
// operator* (12.7× faster than the scalar template path on AVX2; bit-exact parity preserved). Other Mat<T>
// instantiations stay scalar. See mat_simd_f32.hpp for the rationale. Always include this header, not
// mat_types.hpp alone, so every translation unit sees the same overload set.
#include <crd/math/mat_types.hpp>
#include <crd/math/mat_simd_f32.hpp>
