#pragma once

// ckir_harness.hpp — Phase 3.1.6 v17-a: the GPU↔CPU ORACLE-HARNESS primitives. Every CKIR backend (Vulkan/CUDA/…)
// proves against the CPU reference (`ckir_eval.hpp`) with these: bit-exact equality (T1/T3 determinism) and ULP
// distance (the tolerance tier where an op's algorithm legitimately differs). Also determinism helpers (compare N
// repeated runs). The single oracle is the CPU reference; these are how a kernel is judged correct. ADR-0098 DoD §6.

#include <crd/core/types.hpp>

#include <cstring>

namespace crd::kir
{

// total-order monotonic remap of an f64 (Bruce-Dawson): finite doubles map to a monotonically increasing u64, so a
// simple difference is the ULP distance. (±0 differ by 1 ULP — use bit_equal for strict bit determinism.)
[[nodiscard]] inline crd::u64 mono_order(crd::f64 x) noexcept
{
    crd::u64 u = 0;
    std::memcpy(&u, &x, sizeof(u));
    return (u & 0x8000000000000000ULL) != 0 ? ~u : (u | 0x8000000000000000ULL);
}

[[nodiscard]] inline crd::u64 ulp_distance(crd::f64 a, crd::f64 b) noexcept
{
    const crd::u64 ma = mono_order(a);
    const crd::u64 mb = mono_order(b);
    return ma > mb ? ma - mb : mb - ma;
}

// bit-exact equality of two f64 buffers (the T1/T3 determinism gate).
[[nodiscard]] inline bool bit_equal(const crd::f64* a, const crd::f64* b, crd::i64 n) noexcept
{
    for (crd::i64 i = 0; i < n; ++i)
    {
        crd::u64 ua = 0;
        crd::u64 ub = 0;
        std::memcpy(&ua, &a[i], sizeof(ua));
        std::memcpy(&ub, &b[i], sizeof(ub));
        if (ua != ub) { return false; }
    }
    return true;
}

[[nodiscard]] inline crd::u64 max_ulp(const crd::f64* a, const crd::f64* b, crd::i64 n) noexcept
{
    crd::u64 m = 0;
    for (crd::i64 i = 0; i < n; ++i) { const crd::u64 d = ulp_distance(a[i], b[i]); if (d > m) { m = d; } }
    return m;
}

[[nodiscard]] inline bool within_ulp(const crd::f64* a, const crd::f64* b, crd::i64 n, crd::u64 tol) noexcept
{
    return max_ulp(a, b, n) <= tol;
}

[[nodiscard]] inline crd::f64 max_abs_diff(const crd::f64* a, const crd::f64* b, crd::i64 n) noexcept
{
    crd::f64 m = 0.0;
    for (crd::i64 i = 0; i < n; ++i) { const crd::f64 d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]; if (d > m) { m = d; } }
    return m;
}

[[nodiscard]] inline crd::f64 max_rel_diff(const crd::f64* a, const crd::f64* b, crd::i64 n) noexcept
{
    crd::f64 m = 0.0;
    for (crd::i64 i = 0; i < n; ++i)
    {
        const crd::f64 d  = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        const crd::f64 av = a[i] < 0.0 ? -a[i] : a[i];
        const crd::f64 rd = av > 0.0 ? d / av : d;
        if (rd > m) { m = rd; }
    }
    return m;
}

} // namespace crd::kir
