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

// Per-type register-tile dimensions (MR rows × NR cols), v5a-4 microkernel-tune.
// f64 uses a 6×8 single-pass AVX2 tile (12 Vec4d accumulators, ~98% peak standalone) —
// strictly better than the old 8×8-as-2×(8×4) (86%, k-loop run twice). f32 / complex
// keep 8×8 (default). pack_a / pack_b / gemm_packed_inner / the gemm driver / syrk all
// read GemmTraits<T>::MR/NR so the packed layout matches the microkernel per type.
// BIT-IDENTICAL across tile shapes: every C[i][j] is still Σ_p a[i][p]·b[p][j] in p-order
// — only the register tiling changes, never the math ⇒ zero blast radius on values.
template <typename T> struct GemmTraits
{
    static constexpr crd::usize MR = 8;
    static constexpr crd::usize NR = 8;
};
template <> struct GemmTraits<crd::f64>
{
    static constexpr crd::usize MR = 6;
    static constexpr crd::usize NR = 8;
};

// Scalar microkernel — works for any T and any MR×NR tile. Loads C, accumulates K, stores.
// `lda` is the A row stride (== k for the standard packed Ac layout; the packed-TRSM panel
// walk passes lda = obw to read a K-slice of a wider resident row — same math, same p-order).
template <typename T, crd::usize MR, crd::usize NR>
inline void gemm_microkernel_scalar(crd::usize k, crd::usize lda,
                                    const T* a_packed, // MR × K row-major (row stride lda)
                                    const T* b_packed, // K  × NR row-major
                                    T* c_tile,         // MR × NR strided
                                    crd::usize ldc) noexcept
{
    T c[MR][NR]{};
    // Load existing C into accumulators.
    for (crd::usize i = 0; i < MR; ++i)
    {
        for (crd::usize j = 0; j < NR; ++j)
        {
            c[i][j] = c_tile[i * ldc + j];
        }
    }
    // Accumulate K iterations.
    for (crd::usize p = 0; p < k; ++p)
    {
        for (crd::usize i = 0; i < MR; ++i)
        {
            const T a_ip = a_packed[i * lda + p];
            for (crd::usize j = 0; j < NR; ++j)
            {
                c[i][j] = c[i][j] + a_ip * b_packed[p * NR + j];
            }
        }
    }
    // Store back to C.
    for (crd::usize i = 0; i < MR; ++i)
    {
        for (crd::usize j = 0; j < NR; ++j)
        {
            c_tile[i * ldc + j] = c[i][j];
        }
    }
}

#if CRD_SIMD_HAS_AVX2
// Software-prefetch hint (read / T0). v5a-4 gemm lift: the full-gemm gap vs the ~98% L1-resident
// standalone rate is the COLD next B-micropanel — re-streamed from L3 once per m-panel (the Bc
// block is L3-resident, the a-panel L2-resident + reused). The HW prefetcher only partly hides the
// per-panel L3 latency. Prefetching the NEXT b-panel's lines SPREAD across the current k-loop lands
// it in L1 by the next microkernel call. Pure hint ⇒ ZERO value change (bit-identical) ⇒ the
// determinism moat + residual are unchanged. (Goto/BLIS software-prefetch — the asm-vs-intrinsics
// gap; lifts ALL hesap-dense gemm. Past-the-end prefetch on the last panel is a harmless no-op.)
inline void gemm_prefetch_t0(const void* p) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(p, 0, 3);
#elif defined(_MSC_VER)
    _mm_prefetch(reinterpret_cast<const char*>(p), _MM_HINT_T0);
#else
    (void)p;
#endif
}

// AVX2 microkernel for f64 — 6×8 SINGLE-PASS tile (v5a-4 microkernel-tune). 12 Vec4d
// accumulators (6 rows × 2 col-halves) + 2 B-loads + 6 A-broadcasts per k. The k-loop runs
// ONCE (vs the old 8×8-as-2×(8×4) which ran it twice, re-reading A). 0.67 load:FMA and 12
// independent FMA chains fully hide the FMA latency across the 2 ports ⇒ ~98% of f64 peak
// standalone (build/_uk_spike.cpp), vs 86% for the old 8×8. MR=6 (= GemmTraits<f64>::MR);
// pack_a feeds 6-row panels. Mul/add fuse via single-rounded simd::fma (ADR-0082); each
// C[i][j] = Σ_p a[i][p]·b[p][j] in p-order ⇒ bit-identical to the old tile.
template <bool ZeroInit = false>
inline void
gemm_microkernel_avx2_f64(crd::usize k,
                          crd::usize lda, // A row stride (== k for packed Ac; obw for the packed-TRSM resident panel)
                          const crd::f64* a_packed, const crd::f64* b_packed, crd::f64* c_tile, crd::usize ldc) noexcept
{
    namespace simd = crd::math::simd;

    // Load existing C: 6 rows × (2 Vec4d halves). ZeroInit: the caller's tile
    // is known-zero — start the accumulators at 0 in registers instead
    // (identical bits; elides the zero-store pass AND this zero-load pass).
    simd::Vec4d c00 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 0 * ldc + 0);
    simd::Vec4d c01 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 0 * ldc + 4);
    simd::Vec4d c10 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 1 * ldc + 0);
    simd::Vec4d c11 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 1 * ldc + 4);
    simd::Vec4d c20 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 2 * ldc + 0);
    simd::Vec4d c21 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 2 * ldc + 4);
    simd::Vec4d c30 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 3 * ldc + 0);
    simd::Vec4d c31 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 3 * ldc + 4);
    simd::Vec4d c40 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 4 * ldc + 0);
    simd::Vec4d c41 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 4 * ldc + 4);
    simd::Vec4d c50 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 5 * ldc + 0);
    simd::Vec4d c51 = ZeroInit ? simd::Vec4d(0.0) : simd::Vec4d::load(c_tile + 5 * ldc + 4);

    for (crd::usize p = 0; p < k; ++p)
    {
        const simd::Vec4d b0 = simd::Vec4d::load(b_packed + p * 8 + 0); // NR = 8 (GemmTraits<f64>::NR)
        const simd::Vec4d b1 = simd::Vec4d::load(b_packed + p * 8 + 4);
        // Prefetch line p of the NEXT b-panel (contiguous at b_packed + k·8 in the packed Bc buffer),
        // spread one 64-byte line per k-iteration so the whole 16 KB next panel lands in L1 by the
        // next microkernel call — hiding the L3 re-stream latency that the HW prefetcher can't fully
        // cover at each panel boundary. Past-the-end on the last panel is a harmless no-op hint.
        gemm_prefetch_t0(b_packed + (k + p) * 8);
        // ONE reused A-broadcast register: broadcast row i, do both C-halves, discard.
        // Keeps the live set at 12 accumulators + 2 B + 1 A = 15 of 16 YMM (no spill).
        // Single-rounded AVX2 FMA per ADR-0082; each C[i][j] = Σ_p a[i][p]·b[p][j] in p-order.
        simd::Vec4d a = simd::Vec4d(a_packed[0 * lda + p]);
        c00 = simd::fma(a, b0, c00);
        c01 = simd::fma(a, b1, c01);
        a = simd::Vec4d(a_packed[1 * lda + p]);
        c10 = simd::fma(a, b0, c10);
        c11 = simd::fma(a, b1, c11);
        a = simd::Vec4d(a_packed[2 * lda + p]);
        c20 = simd::fma(a, b0, c20);
        c21 = simd::fma(a, b1, c21);
        a = simd::Vec4d(a_packed[3 * lda + p]);
        c30 = simd::fma(a, b0, c30);
        c31 = simd::fma(a, b1, c31);
        a = simd::Vec4d(a_packed[4 * lda + p]);
        c40 = simd::fma(a, b0, c40);
        c41 = simd::fma(a, b1, c41);
        a = simd::Vec4d(a_packed[5 * lda + p]);
        c50 = simd::fma(a, b0, c50);
        c51 = simd::fma(a, b1, c51);
    }

    c00.store(c_tile + 0 * ldc + 0);
    c01.store(c_tile + 0 * ldc + 4);
    c10.store(c_tile + 1 * ldc + 0);
    c11.store(c_tile + 1 * ldc + 4);
    c20.store(c_tile + 2 * ldc + 0);
    c21.store(c_tile + 2 * ldc + 4);
    c30.store(c_tile + 3 * ldc + 0);
    c31.store(c_tile + 3 * ldc + 4);
    c40.store(c_tile + 4 * ldc + 0);
    c41.store(c_tile + 4 * ldc + 4);
    c50.store(c_tile + 5 * ldc + 0);
    c51.store(c_tile + 5 * ldc + 4);
}

// AVX2 microkernel for f32. Uses Vec8f registers for the 8-wide NR axis.
// Tile shape: MR=8 × NR=8. Holds 8 Vec8f register accumulators for C
// rows, broadcasts A[i,p] as needed, loads B[p,:] as a Vec8f.
template <bool ZeroInit = false>
inline void gemm_microkernel_avx2_f32(crd::usize k,
                                      crd::usize lda, // A row stride (== k for packed Ac)
                                      const crd::f32* a_packed, const crd::f32* b_packed, crd::f32* c_tile,
                                      crd::usize ldc) noexcept
{
    namespace simd = crd::math::simd;

    // Load existing C into 8 Vec8f registers (ZeroInit: start at 0 — same bits).
    simd::Vec8f c0 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 0 * ldc);
    simd::Vec8f c1 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 1 * ldc);
    simd::Vec8f c2 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 2 * ldc);
    simd::Vec8f c3 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 3 * ldc);
    simd::Vec8f c4 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 4 * ldc);
    simd::Vec8f c5 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 5 * ldc);
    simd::Vec8f c6 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 6 * ldc);
    simd::Vec8f c7 = ZeroInit ? simd::Vec8f(0.0F) : simd::Vec8f::load(c_tile + 7 * ldc);

    for (crd::usize p = 0; p < k; ++p)
    {
        // Broadcast each A row's p-th element.
        const simd::Vec8f a0(a_packed[0 * lda + p]);
        const simd::Vec8f a1(a_packed[1 * lda + p]);
        const simd::Vec8f a2(a_packed[2 * lda + p]);
        const simd::Vec8f a3(a_packed[3 * lda + p]);
        const simd::Vec8f a4(a_packed[4 * lda + p]);
        const simd::Vec8f a5(a_packed[5 * lda + p]);
        const simd::Vec8f a6(a_packed[6 * lda + p]);
        const simd::Vec8f a7(a_packed[7 * lda + p]);
        // Load B[p, 0..7] as one Vec8f.
        const simd::Vec8f bp = simd::Vec8f::load(b_packed + p * kGemmNr);
        // Prefetch line p of the NEXT b-panel (b_packed + k·NR), spread across the k-loop so it
        // lands in L1 by the next microkernel call. See the f64 kernel for the full rationale.
        gemm_prefetch_t0(b_packed + (k + p) * kGemmNr);

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
// FUSED-MERGE kernels (E4 2026-07-03): the packed-inner pipeline previously
// stored the finished tile to a stack buffer and re-read it in a merge loop
// (`c += alpha*micro`). For FULL tiles the fused kernel updates the real C
// directly from the accumulator REGISTERS with the EXACT same elementwise
// operation sequence (mul then add — two roundings — or a single add for
// alpha==1), so the results are bit-identical; ~96 memory ops per tile call
// disappear. Partial edge tiles keep the micro+merge path.
template <bool Alpha1>
inline void gemm_microkernel_avx2_f64_fused(crd::usize k, const crd::f64* a_packed, const crd::f64* b_packed,
                                            crd::f64* c, crd::usize ldc, crd::f64 alpha) noexcept
{
    namespace simd = crd::math::simd;
    // BLIS-style C prefetch: the merge loads C AFTER the whole k-loop — issue
    // the line requests now so they overlap the compute (order-free hint).
    for (crd::usize i = 0; i < 6U; ++i)
    {
        gemm_prefetch_t0(c + i * ldc);
        gemm_prefetch_t0(c + i * ldc + 4);
    }
    simd::Vec4d c00(0.0), c01(0.0), c10(0.0), c11(0.0), c20(0.0), c21(0.0);
    simd::Vec4d c30(0.0), c31(0.0), c40(0.0), c41(0.0), c50(0.0), c51(0.0);
    for (crd::usize p = 0; p < k; ++p)
    {
        const simd::Vec4d b0 = simd::Vec4d::load(b_packed + p * 8 + 0);
        const simd::Vec4d b1 = simd::Vec4d::load(b_packed + p * 8 + 4);
        gemm_prefetch_t0(b_packed + (k + p) * 8);
        simd::Vec4d a = simd::Vec4d(a_packed[0 * k + p]);
        c00 = simd::fma(a, b0, c00);
        c01 = simd::fma(a, b1, c01);
        a = simd::Vec4d(a_packed[1 * k + p]);
        c10 = simd::fma(a, b0, c10);
        c11 = simd::fma(a, b1, c11);
        a = simd::Vec4d(a_packed[2 * k + p]);
        c20 = simd::fma(a, b0, c20);
        c21 = simd::fma(a, b1, c21);
        a = simd::Vec4d(a_packed[3 * k + p]);
        c30 = simd::fma(a, b0, c30);
        c31 = simd::fma(a, b1, c31);
        a = simd::Vec4d(a_packed[4 * k + p]);
        c40 = simd::fma(a, b0, c40);
        c41 = simd::fma(a, b1, c41);
        a = simd::Vec4d(a_packed[5 * k + p]);
        c50 = simd::fma(a, b0, c50);
        c51 = simd::fma(a, b1, c51);
    }
    // merge from registers: same elementwise IEEE sequence as the old loop
    const simd::Vec4d av(alpha);
    const auto merge = [&](crd::f64* row, simd::Vec4d lo, simd::Vec4d hi) noexcept
    {
        if constexpr (Alpha1)
        {
            (simd::Vec4d::load(row + 0) + lo).store(row + 0);
            (simd::Vec4d::load(row + 4) + hi).store(row + 4);
        }
        else
        {
            (simd::Vec4d::load(row + 0) + av * lo).store(row + 0); // mul, then add: two roundings,
            (simd::Vec4d::load(row + 4) + av * hi).store(row + 4); // exactly like the scalar merge
        }
    };
    merge(c + 0 * ldc, c00, c01);
    merge(c + 1 * ldc, c10, c11);
    merge(c + 2 * ldc, c20, c21);
    merge(c + 3 * ldc, c30, c31);
    merge(c + 4 * ldc, c40, c41);
    merge(c + 5 * ldc, c50, c51);
}

template <bool Alpha1>
inline void gemm_microkernel_avx2_f32_fused(crd::usize k, const crd::f32* a_packed, const crd::f32* b_packed,
                                            crd::f32* c, crd::usize ldc, crd::f32 alpha) noexcept
{
    namespace simd = crd::math::simd;
    for (crd::usize i = 0; i < 8U; ++i) // BLIS-style C prefetch (see the f64 fused kernel)
    {
        gemm_prefetch_t0(c + i * ldc);
    }
    simd::Vec8f c0(0.0F), c1(0.0F), c2(0.0F), c3(0.0F), c4(0.0F), c5(0.0F), c6(0.0F), c7(0.0F);
    for (crd::usize p = 0; p < k; ++p)
    {
        const simd::Vec8f bp = simd::Vec8f::load(b_packed + p * kGemmNr);
        gemm_prefetch_t0(b_packed + (k + p) * kGemmNr);
        c0 = simd::fma(simd::Vec8f(a_packed[0 * k + p]), bp, c0);
        c1 = simd::fma(simd::Vec8f(a_packed[1 * k + p]), bp, c1);
        c2 = simd::fma(simd::Vec8f(a_packed[2 * k + p]), bp, c2);
        c3 = simd::fma(simd::Vec8f(a_packed[3 * k + p]), bp, c3);
        c4 = simd::fma(simd::Vec8f(a_packed[4 * k + p]), bp, c4);
        c5 = simd::fma(simd::Vec8f(a_packed[5 * k + p]), bp, c5);
        c6 = simd::fma(simd::Vec8f(a_packed[6 * k + p]), bp, c6);
        c7 = simd::fma(simd::Vec8f(a_packed[7 * k + p]), bp, c7);
    }
    const simd::Vec8f av(alpha);
    const auto merge = [&](crd::f32* row, simd::Vec8f acc) noexcept
    {
        if constexpr (Alpha1)
        {
            (simd::Vec8f::load(row) + acc).store(row);
        }
        else
        {
            (simd::Vec8f::load(row) + av * acc).store(row);
        }
    };
    merge(c + 0 * ldc, c0);
    merge(c + 1 * ldc, c1);
    merge(c + 2 * ldc, c2);
    merge(c + 3 * ldc, c3);
    merge(c + 4 * ldc, c4);
    merge(c + 5 * ldc, c5);
    merge(c + 6 * ldc, c6);
    merge(c + 7 * ldc, c7);
}

// lda overload: A row stride decoupled from k (the packed-TRSM resident-panel walk reads a K-slice
// of a wider row-major panel). Same math, same p-order — lda==k is the standard packed-Ac case.
template <typename T, bool ZeroInit = false>
inline void gemm_microkernel(crd::usize k, crd::usize lda, const T* a_packed, const T* b_packed, T* c_tile,
                             crd::usize ldc) noexcept
{
#if CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS
#if CRD_SIMD_HAS_AVX2
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        gemm_microkernel_avx2_f32<ZeroInit>(k, lda, a_packed, b_packed, c_tile, ldc);
    }
    else if constexpr (std::is_same_v<T, crd::f64>)
    {
        gemm_microkernel_avx2_f64<ZeroInit>(k, lda, a_packed, b_packed, c_tile, ldc);
    }
    else
    {
        if constexpr (ZeroInit)
        {
            for (crd::usize i = 0; i < GemmTraits<T>::MR; ++i)
            {
                for (crd::usize j = 0; j < GemmTraits<T>::NR; ++j)
                {
                    c_tile[i * ldc + j] = T{};
                }
            }
        }
        gemm_microkernel_scalar<T, GemmTraits<T>::MR, GemmTraits<T>::NR>(k, lda, a_packed, b_packed, c_tile, ldc);
    }
#else
    if constexpr (ZeroInit)
    {
        for (crd::usize i = 0; i < GemmTraits<T>::MR; ++i)
        {
            for (crd::usize j = 0; j < GemmTraits<T>::NR; ++j)
            {
                c_tile[i * ldc + j] = T{};
            }
        }
    }
    gemm_microkernel_scalar<T, GemmTraits<T>::MR, GemmTraits<T>::NR>(k, lda, a_packed, b_packed, c_tile, ldc);
#endif
#else
    // Reserved ASM backend; see detail/microkernel_backend.hpp.
    static_assert(sizeof(T) == 0,
                  "CRD_HESAP_MICROKERNEL_BACKEND_ASM is reserved-but-unimplemented; see ADR-0082 §revisit");
#endif
}

template <typename T, bool ZeroInit = false>
inline void gemm_microkernel(crd::usize k, const T* a_packed, const T* b_packed, T* c_tile, crd::usize ldc) noexcept
{
    gemm_microkernel<T, ZeroInit>(k, k, a_packed, b_packed, c_tile, ldc);
}

// Fused-merge availability + dispatch (full tiles, RowMajor C). Returns
// compile-time false for types without an AVX2 fused kernel so the caller
// keeps the micro+merge path.
template <typename T>
inline constexpr bool kHasFusedMicrokernel =
#if CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS && CRD_SIMD_HAS_AVX2
    std::is_same_v<T, crd::f32> || std::is_same_v<T, crd::f64>;
#else
    false;
#endif

template <typename T, bool Alpha1>
inline void gemm_microkernel_fused(crd::usize k, const T* a_packed, const T* b_packed, T* c, crd::usize ldc,
                                   T alpha) noexcept
{
#if CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS && CRD_SIMD_HAS_AVX2
    if constexpr (std::is_same_v<T, crd::f32>)
    {
        gemm_microkernel_avx2_f32_fused<Alpha1>(k, a_packed, b_packed, c, ldc, alpha);
    }
    else if constexpr (std::is_same_v<T, crd::f64>)
    {
        gemm_microkernel_avx2_f64_fused<Alpha1>(k, a_packed, b_packed, c, ldc, alpha);
    }
#else
    (void)k;
    (void)a_packed;
    (void)b_packed;
    (void)c;
    (void)ldc;
    (void)alpha;
#endif
}

} // namespace crd::hesap::dense::detail
