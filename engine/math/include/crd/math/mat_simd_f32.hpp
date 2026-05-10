// Mat4f SIMD specializations — Phase 3.1 v0f.
//
// Routes the scalar `crd::math::Mat4<f32> operator*` through `simd::Mat4f`
// when the SIMD backend is non-scalar. **12.7× measured speedup** on
// win-release AVX2 (per v0e bench: 122.97 ns scalar vs 9.65 ns SIMD).
//
// Why only Mat4f and not Vec3f / Quatf:
//   - Mat4f is 64 bytes, naturally 16-byte-aligned, fills SIMD registers
//     completely (4 columns × 4 lanes, zero waste). Mat4*Mat4 does
//     16+ multiply-adds, amortising SIMD register load cost completely.
//   - Vec3f is 12 bytes — SIMD-ifying wastes 33% memory + 1 lane per op.
//   - Quatf per-instance SIMD is empirically a 0.65× regression
//     (v0e bench): single Hamilton product doesn't amortise the
//     stack-roundtrip overhead.
//   - Compiler auto-vec already SIMDifies tight loops over scalar Vec3f
//     / Quatf; we don't lose anything by keeping them scalar.
//   - For batched physics, eylem v1+ uses `simd::Soa<BodyChunk8>` with
//     `Vec8f` columns — 8 bodies per AVX op, zero waste.
//
// See `docs/sessions/2026-05-10-v0e-bench-harness.md` for benchmark data
// and ADR-0066 §13 for the wider AoSoA-vs-AoS-SIMD architectural call.
//
// **Bit-exact parity with the scalar template path is contractual.** Same
// accumulation order (left-to-right column sweep), same `mul_add` two-
// rounding semantics (no hardware FMA). Tested in test_math.cpp.

#pragma once

#include <crd/math/simd/backend.hpp>
#include <crd/math/simd/mat4f.hpp>
#include <crd/math/simd/vec4f.hpp>

namespace crd::math
{
// Forward decls — the actual struct definitions live in mat.hpp; this header
// is included from mat.hpp's tail so the templates are already visible by
// the time these specializations are seen.

#if CRD_SIMD_BACKEND != CRD_SIMD_BACKEND_SCALAR

// Mat4<f32> * Vec4<f32> — non-template overload, picked over the template
// for the exact `Mat4<f32> * Vec4<f32>` match. Other `Mat4<T> * Vec4<T>`
// instantiations (e.g. T = f64) still go through the scalar template above.
[[nodiscard]] CRD_FORCEINLINE Vec4<crd::f32> operator*(const Mat4<crd::f32>& lhs,
                                                       const Vec4<crd::f32>& rhs) noexcept
{
    // Build SIMD columns from the scalar storage. simd::Vec4f's 4-arg
    // constructor uses _mm_set_ps (or scalar-fallback equivalent) — same
    // cost as a single load, no alignment requirement on Vec4<f32>.
    const simd::Vec4f c0(lhs.c0.x, lhs.c0.y, lhs.c0.z, lhs.c0.w);
    const simd::Vec4f c1(lhs.c1.x, lhs.c1.y, lhs.c1.z, lhs.c1.w);
    const simd::Vec4f c2(lhs.c2.x, lhs.c2.y, lhs.c2.z, lhs.c2.w);
    const simd::Vec4f c3(lhs.c3.x, lhs.c3.y, lhs.c3.z, lhs.c3.w);

    // Linear combination: r = c0*x + c1*y + c2*z + c3*w (same order as
    // the scalar template path; bit-exact across backends per ADR-0063).
    simd::Vec4f r = c0 * simd::Vec4f(rhs.x);
    r = simd::mul_add(c1, simd::Vec4f(rhs.y), r);
    r = simd::mul_add(c2, simd::Vec4f(rhs.z), r);
    r = simd::mul_add(c3, simd::Vec4f(rhs.w), r);

    crd::f32 lanes[4];
    r.store(lanes);
    return Vec4<crd::f32>{lanes[0], lanes[1], lanes[2], lanes[3]};
}

// Mat4<f32> * Mat4<f32> — non-template overload. Computes column-wise as
// 4 mat-vec multiplies; each reuses the SIMD-loaded `lhs` columns,
// amortising the SIMD register-fill cost across the whole operation.
[[nodiscard]] CRD_FORCEINLINE Mat4<crd::f32> operator*(const Mat4<crd::f32>& lhs,
                                                       const Mat4<crd::f32>& rhs) noexcept
{
    // Pre-load lhs's columns once into SIMD regs; reuse for each result column.
    const simd::Vec4f l0(lhs.c0.x, lhs.c0.y, lhs.c0.z, lhs.c0.w);
    const simd::Vec4f l1(lhs.c1.x, lhs.c1.y, lhs.c1.z, lhs.c1.w);
    const simd::Vec4f l2(lhs.c2.x, lhs.c2.y, lhs.c2.z, lhs.c2.w);
    const simd::Vec4f l3(lhs.c3.x, lhs.c3.y, lhs.c3.z, lhs.c3.w);

    // result.col[j] = l0*rhs.cj.x + l1*rhs.cj.y + l2*rhs.cj.z + l3*rhs.cj.w
    auto mul_col = [&](const Vec4<crd::f32>& v) noexcept -> Vec4<crd::f32>
    {
        simd::Vec4f r = l0 * simd::Vec4f(v.x);
        r = simd::mul_add(l1, simd::Vec4f(v.y), r);
        r = simd::mul_add(l2, simd::Vec4f(v.z), r);
        r = simd::mul_add(l3, simd::Vec4f(v.w), r);
        crd::f32 lanes[4];
        r.store(lanes);
        return Vec4<crd::f32>{lanes[0], lanes[1], lanes[2], lanes[3]};
    };

    return Mat4<crd::f32>(mul_col(rhs.c0), mul_col(rhs.c1),
                          mul_col(rhs.c2), mul_col(rhs.c3));
}

#endif // CRD_SIMD_BACKEND != CRD_SIMD_BACKEND_SCALAR

} // namespace crd::math
