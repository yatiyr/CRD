// crd-geometry-primitives — SIMD-batched hull support (v2h).
//
// `support_simd_f32(hull, dir)` is the AVX2 `Vec8f` path for
// `ConvexHullView<f32>` support evaluation, dispatched from
// `support_with_hint(hull, dir, hint)` when the hull carries the optional
// SoA fields and `vertices.size() <= k_simd_support_threshold` (= 32).
//
// **Algorithm**: broadcast `dir.x/y/z` to `Vec8f`; loop chunks of 8
// vertices loading `vx`/`vy`/`vz` from the hull's SoA arrays; compute
// `proj = vx*dx + vy*dy + vz*dz` (one mul-add chain, 8 dots in parallel);
// scalar reduce the 8 lanes to (best_proj, best_idx) with strict-greater
// + lowest-index tiebreak.
//
// **Determinism**: scalar reducer with `proj > best_proj` (strict greater).
// Identical convention to the linear-scan `support(hull, dir)` — pairs
// differing by 1 ULP are treated as DISTINCT (the strictly-higher one
// wins); exact ties go to the lower index. The contract test in
// `test_simd_support.cpp` verifies this on 1000 random directions
// (`support_simd_f32(hull, dir).vertex_idx == support(hull, dir).vertex_idx`).
//
// **Padding contract**: caller MUST pad `vx_soa`/`vy_soa`/`vz_soa` to the
// next multiple of 8 by repeating vertex 0's coordinates in lanes
// `[n, padded_size)`. Padded lanes contribute `dot(vertex_0, dir)` —
// tied with lane 0, which wins by lowest-index tiebreak. No per-chunk
// branch on `n_remaining`; the scan is branch-free.
//
// **Out-of-line** (this .cpp) so the AVX2 `ymm` instructions are emitted
// in a real .obj. Mirrors v0f's `simd_batch.cpp` pattern. The existing
// `crd-simd-emission-check` on `test_simd.cpp.obj` covers the substrate;
// this TU follows the same compilation flags (`/arch:AVX2` via the
// `crd-simd-flags` target inherited through `crd-geometry-primitives`).

#include <crd/geometry/primitives/primitives.hpp>
#include <crd/math/simd/vec8f.hpp>

#include <cstring>
#include <limits>

namespace crd::geometry::primitives
{

SupportPoint<crd::f32> support_simd_f32(const ConvexHullView<crd::f32>& hull, const Vec3<crd::f32>& dir) noexcept
{
    CRD_ASSERT(!hull.vertices.empty());
    CRD_ASSERT(!hull.vx_soa.empty());
    CRD_ASSERT(hull.vx_soa.size() == hull.vy_soa.size());
    CRD_ASSERT(hull.vx_soa.size() == hull.vz_soa.size());

    [[maybe_unused]] const crd::usize n = hull.vertices.size();
    const crd::usize padded = hull.vx_soa.size();
    CRD_ASSERT(padded >= n);
    CRD_ASSERT((padded & 7U) == 0U); // padded to multiple of 8 per the contract

    const crd::math::simd::Vec8f dx(dir.x);
    const crd::math::simd::Vec8f dy(dir.y);
    const crd::math::simd::Vec8f dz(dir.z);

    crd::f32 best_proj = -std::numeric_limits<crd::f32>::infinity();
    crd::u32 best_idx = 0U;

    alignas(32) crd::f32 proj_lanes[8];

    for (crd::usize base = 0; base < padded; base += 8)
    {
        const crd::math::simd::Vec8f vx = crd::math::simd::Vec8f::load(&hull.vx_soa[base]);
        const crd::math::simd::Vec8f vy = crd::math::simd::Vec8f::load(&hull.vy_soa[base]);
        const crd::math::simd::Vec8f vz = crd::math::simd::Vec8f::load(&hull.vz_soa[base]);
        // `mul_add(a, b, c) = a*b + c`. Chain two of them for the 3-component dot:
        //   proj = vx*dx + vy*dy + vz*dz
        //        = mul_add(vz, dz, mul_add(vy, dy, vx*dx))
        // The chain is fully associative + commutative at the IEEE-no-FMA level
        // (we don't use `_mm256_fmadd_ps` — see `crd-math` ADR-0063 / the
        // determinism contract; `mul_add` here is `(a*b)+c` as two separate
        // ops, not a fused FMA).
        const crd::math::simd::Vec8f proj = crd::math::simd::mul_add(
            vz, dz, crd::math::simd::mul_add(vy, dy, vx * dx));
        proj.store(proj_lanes);
        // Scalar reduce — match linear-scan's strict-greater + lowest-index
        // tiebreak. Branchless wouldn't be deterministic without a 4-key
        // comparator (see `reduce_argmax_with_lex_tiebreak`); for v2h's
        // simpler "lowest index" rule a 1-key scalar scan is the cleanest
        // way to stay bit-identical to the AoS path.
        for (crd::u32 lane = 0; lane < 8U; ++lane)
        {
            const crd::f32 p = proj_lanes[lane];
            if (p > best_proj)
            {
                best_proj = p;
                best_idx = static_cast<crd::u32>(base) + lane;
            }
            // Note: on exact-tie (p == best_proj), keep the existing
            // best_idx (lower because we iterate ascending) — matches
            // linear scan.
        }
    }
    // The padded lanes carry vertex 0's coords (per the padding contract);
    // their `base + lane` indices are ≥ n, so they LOSE the lowest-index
    // tiebreak against the real lane 0 — and the real maxima at indices
    // < n always beat them on score (or tie and win on lowest index).
    // So `best_idx` is guaranteed to be in [0, n).
    CRD_ASSERT(best_idx < n);
    return SupportPoint<crd::f32>{hull.vertices[best_idx], best_idx};
}

} // namespace crd::geometry::primitives
