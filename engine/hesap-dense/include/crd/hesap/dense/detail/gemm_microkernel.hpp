#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/dense/detail/microkernel_backend.hpp>
#include <crd/math/simd/backend.hpp>

#include <type_traits>

#if CRD_SIMD_HAS_AVX2
#include <crd/math/simd/vec4d.hpp>
#include <crd/math/simd/vec8f.hpp>
#endif

namespace crd::hesap::dense::detail
{
// -----------------------------------------------------------------------
// gemm microkernel — Goto/BLIS leaf. Phase 3.1.6 v0d.
//
// THIS IS THE HOT-SWAP POINT for future ASM microkernel work per ADR-0082.
// The `gemm_microkernel<T>` dispatcher signature at the bottom of this
// file is LOCKED and matches the contract any future per-µarch ASM
// implementation must export. Today (Intrinsics backend, ADR-0082 §decision)
// the dispatcher routes to scalar / AVX2 / future AVX-512 / future NEON
// kernels written in C++. A future ASM backend would route to extern
// symbols defined in `engine/hesap-dense/src/asm/`.
//
// See detail/microkernel_backend.hpp for the backend switch + the
// three-condition revisit gate.
//
// Microkernel computes:
//   for r = 0..MR:
//     for c = 0..NR:
//       C[r,c] += sum_{k=0..K-1} A[r,k] * B[k,c]
//
// where A is packed as MR consecutive rows × K cols (row-major), B is
// packed as K rows × NR cols (row-major), and C is the destination
// tile [MR × NR] with stride `ldc`. THIS PACKING LAYOUT is per-backend
// — when ASM lands, BLIS-convention packing comes with it.
//
// Block sizes are chosen for AVX2: MR = 8, NR = 8.
// -----------------------------------------------------------------------

inline constexpr crd::usize kGemmMr = 8;
inline constexpr crd::usize kGemmNr = 8;

// Scalar microkernel — works for any T. Loops fully unrolled for MR=NR=8.
// Accumulates into local registers then writes C once per tile.
template <typename T>
inline void gemm_microkernel_scalar(
    crd::usize k,
    const T* a_packed,    // MR × K row-major
    const T* b_packed,    // K  × NR row-major
    T*       c_tile,      // MR × NR strided
    crd::usize ldc) noexcept
{
    T c[kGemmMr][kGemmNr]{};
    // Load existing C into accumulators.
    for (crd::usize i = 0; i < kGemmMr; ++i)
    {
        for (crd::usize j = 0; j < kGemmNr; ++j)
        {
            c[i][j] = c_tile[i * ldc + j];
        }
    }
    // Accumulate K iterations.
    for (crd::usize p = 0; p < k; ++p)
    {
        for (crd::usize i = 0; i < kGemmMr; ++i)
        {
            const T a_ip = a_packed[i * k + p];
            for (crd::usize j = 0; j < kGemmNr; ++j)
            {
                c[i][j] = c[i][j] + a_ip * b_packed[p * kGemmNr + j];
            }
        }
    }
    // Store back to C.
    for (crd::usize i = 0; i < kGemmMr; ++i)
    {
        for (crd::usize j = 0; j < kGemmNr; ++j)
        {
            c_tile[i * ldc + j] = c[i][j];
        }
    }
}

#if CRD_SIMD_HAS_AVX2
// AVX2 microkernel for f64. Same MR=8 × NR=8 packed tile as the scalar /
// f32 path; internally processed as 2 × (MR=8 × NR=4) halves since one
// Vec4d holds 4 doubles. Per half: 8 Vec4d row accumulators + 1 broadcast
// A register + 1 B load = 10 of 16 YMM registers (comfortable headroom).
//
// Mul + add are SEPARATE ops per ADR-0063 determinism — Vec4d's
// operator+ / operator* don't fuse (no _mm256_fmadd_pd) so SIMD/scalar
// parity holds bit-exactly.
inline void gemm_microkernel_avx2_f64(
    crd::usize k,
    const crd::f64* a_packed,
    const crd::f64* b_packed,
    crd::f64* c_tile,
    crd::usize ldc) noexcept
{
    namespace simd = crd::math::simd;

    // Process the 8×8 tile as 2 × (8×4) halves. Each half processes 4 cols.
    for (crd::usize half = 0; half < 2; ++half)
    {
        const crd::usize col_offset = half * 4;

        simd::Vec4d c0 = simd::Vec4d::load(c_tile + 0 * ldc + col_offset);
        simd::Vec4d c1 = simd::Vec4d::load(c_tile + 1 * ldc + col_offset);
        simd::Vec4d c2 = simd::Vec4d::load(c_tile + 2 * ldc + col_offset);
        simd::Vec4d c3 = simd::Vec4d::load(c_tile + 3 * ldc + col_offset);
        simd::Vec4d c4 = simd::Vec4d::load(c_tile + 4 * ldc + col_offset);
        simd::Vec4d c5 = simd::Vec4d::load(c_tile + 5 * ldc + col_offset);
        simd::Vec4d c6 = simd::Vec4d::load(c_tile + 6 * ldc + col_offset);
        simd::Vec4d c7 = simd::Vec4d::load(c_tile + 7 * ldc + col_offset);

        // No software prefetch — measured wash vs the HW streaming
        // prefetcher which already handles sequential Bc traversal.
        for (crd::usize p = 0; p < k; ++p)
        {
            const simd::Vec4d bp = simd::Vec4d::load(b_packed + p * kGemmNr + col_offset);
            const simd::Vec4d a0(a_packed[0 * k + p]);
            const simd::Vec4d a1(a_packed[1 * k + p]);
            const simd::Vec4d a2(a_packed[2 * k + p]);
            const simd::Vec4d a3(a_packed[3 * k + p]);
            const simd::Vec4d a4(a_packed[4 * k + p]);
            const simd::Vec4d a5(a_packed[5 * k + p]);
            const simd::Vec4d a6(a_packed[6 * k + p]);
            const simd::Vec4d a7(a_packed[7 * k + p]);

            // Single-rounded AVX2 FMA per ADR-0082 hesap perf path. Bit-exact
            // within hesap (all SIMD widths + scalar use fma()) but not bit-
            // exact with the no-FMA mul_add path of crd-eylem.
            c0 = simd::fma(a0, bp, c0);
            c1 = simd::fma(a1, bp, c1);
            c2 = simd::fma(a2, bp, c2);
            c3 = simd::fma(a3, bp, c3);
            c4 = simd::fma(a4, bp, c4);
            c5 = simd::fma(a5, bp, c5);
            c6 = simd::fma(a6, bp, c6);
            c7 = simd::fma(a7, bp, c7);
        }

        c0.store(c_tile + 0 * ldc + col_offset);
        c1.store(c_tile + 1 * ldc + col_offset);
        c2.store(c_tile + 2 * ldc + col_offset);
        c3.store(c_tile + 3 * ldc + col_offset);
        c4.store(c_tile + 4 * ldc + col_offset);
        c5.store(c_tile + 5 * ldc + col_offset);
        c6.store(c_tile + 6 * ldc + col_offset);
        c7.store(c_tile + 7 * ldc + col_offset);
    }
}

// AVX2 microkernel for f32. Uses Vec8f registers for the 8-wide NR axis.
// Tile shape: MR=8 × NR=8. Holds 8 Vec8f register accumulators for C
// rows, broadcasts A[i,p] as needed, loads B[p,:] as a Vec8f.
inline void gemm_microkernel_avx2_f32(
    crd::usize k,
    const crd::f32* a_packed,
    const crd::f32* b_packed,
    crd::f32* c_tile,
    crd::usize ldc) noexcept
{
    namespace simd = crd::math::simd;

    // Load existing C into 8 Vec8f registers (one per row).
    simd::Vec8f c0 = simd::Vec8f::load(c_tile + 0 * ldc);
    simd::Vec8f c1 = simd::Vec8f::load(c_tile + 1 * ldc);
    simd::Vec8f c2 = simd::Vec8f::load(c_tile + 2 * ldc);
    simd::Vec8f c3 = simd::Vec8f::load(c_tile + 3 * ldc);
    simd::Vec8f c4 = simd::Vec8f::load(c_tile + 4 * ldc);
    simd::Vec8f c5 = simd::Vec8f::load(c_tile + 5 * ldc);
    simd::Vec8f c6 = simd::Vec8f::load(c_tile + 6 * ldc);
    simd::Vec8f c7 = simd::Vec8f::load(c_tile + 7 * ldc);

    for (crd::usize p = 0; p < k; ++p)
    {
        // Broadcast each A row's p-th element.
        const simd::Vec8f a0(a_packed[0 * k + p]);
        const simd::Vec8f a1(a_packed[1 * k + p]);
        const simd::Vec8f a2(a_packed[2 * k + p]);
        const simd::Vec8f a3(a_packed[3 * k + p]);
        const simd::Vec8f a4(a_packed[4 * k + p]);
        const simd::Vec8f a5(a_packed[5 * k + p]);
        const simd::Vec8f a6(a_packed[6 * k + p]);
        const simd::Vec8f a7(a_packed[7 * k + p]);
        // Load B[p, 0..7] as one Vec8f.
        const simd::Vec8f bp = simd::Vec8f::load(b_packed + p * kGemmNr);

        // Single-rounded AVX2 FMA per ADR-0082 hesap perf path.
        c0 = simd::fma(a0, bp, c0);
        c1 = simd::fma(a1, bp, c1);
        c2 = simd::fma(a2, bp, c2);
        c3 = simd::fma(a3, bp, c3);
        c4 = simd::fma(a4, bp, c4);
        c5 = simd::fma(a5, bp, c5);
        c6 = simd::fma(a6, bp, c6);
        c7 = simd::fma(a7, bp, c7);
    }

    c0.store(c_tile + 0 * ldc);
    c1.store(c_tile + 1 * ldc);
    c2.store(c_tile + 2 * ldc);
    c3.store(c_tile + 3 * ldc);
    c4.store(c_tile + 4 * ldc);
    c5.store(c_tile + 5 * ldc);
    c6.store(c_tile + 6 * ldc);
    c7.store(c_tile + 7 * ldc);
}
#endif

// ---- Locked hot-swap dispatcher (ADR-0082) ----------------------------
// Signature contract for both Intrinsics (today) and Asm (reserved)
// backends:
//
//   void gemm_microkernel<T>(usize k, const T* a_packed,
//                            const T* b_packed, T* c_tile, usize ldc) noexcept;
//
// Backend selection is via CRD_HESAP_MICROKERNEL_BACKEND in
// detail/microkernel_backend.hpp. Today only the INTRINSICS branch is
// implemented; the ASM branch is reserved for the future
// v0d-asm-microkernel slice once the three-condition revisit gate
// in ADR-0082 §revisit is satisfied.
template <typename T>
inline void gemm_microkernel(crd::usize k, const T* a_packed, const T* b_packed, T* c_tile, crd::usize ldc) noexcept
{
#if CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS
#if CRD_SIMD_HAS_AVX2
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        gemm_microkernel_avx2_f32(k, a_packed, b_packed, c_tile, ldc);
    }
    else if constexpr (std::is_same_v<T, crd::f64>)
    {
        gemm_microkernel_avx2_f64(k, a_packed, b_packed, c_tile, ldc);
    }
    else
    {
        gemm_microkernel_scalar<T>(k, a_packed, b_packed, c_tile, ldc);
    }
#else
    gemm_microkernel_scalar<T>(k, a_packed, b_packed, c_tile, ldc);
#endif
#else
    // Reserved ASM backend; see detail/microkernel_backend.hpp.
    static_assert(sizeof(T) == 0,
        "CRD_HESAP_MICROKERNEL_BACKEND_ASM is reserved-but-unimplemented; see ADR-0082 §revisit");
#endif
}

} // namespace crd::hesap::dense::detail
