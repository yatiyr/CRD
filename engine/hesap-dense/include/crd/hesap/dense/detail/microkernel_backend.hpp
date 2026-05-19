#pragma once

// -----------------------------------------------------------------------
// Hesap GEMM microkernel backend selection. ADR-0082 Accepted 2026-05-19.
//
// Cerid ships pure C++ intrinsics microkernels via `crd-math::simd::Vec8f`
// (AVX2), `Vec16f` (AVX-512), `Vec4f` (NEON), and a scalar fallback. Per-µarch
// hand-rolled assembly is DEFERRED INDEFINITELY behind this compile-time
// switch; the architecture below preserves a clean hot-swap point for a
// future architect who satisfies the three-condition revisit gate in
// ADR-0082 §revisit.
//
// ---- Locked microkernel signature (ASM impls MUST match) --------------
//
//   template <typename T>
//   void gemm_microkernel(crd::usize k,
//                         const T* a_packed,    // packing layout per-backend
//                         const T* b_packed,    // packing layout per-backend
//                         T*       c_tile,
//                         crd::usize ldc) noexcept;
//
// Tests run against the dispatcher in `gemm_microkernel.hpp`, so the asm
// path can drop in without changing any test code. Packing layout (Ac panel
// + Bc panel format) is backend-coupled — when ASM lands, BLIS-convention
// packing comes with it; the dense GEMM driver doesn't care which packing
// format is in use.
//
// ---- When to flip the backend (ADR-0082 three-condition revisit) -------
//
// All three must hold:
//   1. A measured Cerid workload sustains > 50 % of solve time on GEMM
//      at N > 1000 (GEMM is the bottleneck, not an inner-loop concern).
//   2. The intrinsics microkernel measures < 70 % of MKL/BLIS-asm peak
//      on that workload's hardware.
//   3. No other optimization (GPU offload via crd-rhi-compute, sparse
//      algorithm switch, different solver) has a better cost-benefit ratio.
//
// If all three: open a new ADR superseding 0082, commit to per-µarch ASM
// maintenance (≥ 6 µarchs day 1; CI matrix expansion; ABI guarantees for
// extern ASM symbols), and ship a `v0d-asm-microkernel` slice that adds
// `engine/hesap-dense/src/asm/<arch>.S` files and flips the default below.
//
// Until that day: intrinsics is the elite path for Cerid's engineering
// economics. Same call Eigen / Faer / Highway / xtensor / Stan-math /
// Armadillo / mlpack made; not a shortcut.
// -----------------------------------------------------------------------

#define CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS 1
#define CRD_HESAP_MICROKERNEL_BACKEND_ASM        2

#ifndef CRD_HESAP_MICROKERNEL_BACKEND
#define CRD_HESAP_MICROKERNEL_BACKEND CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS
#endif

static_assert(
    CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_INTRINSICS ||
    CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_ASM,
    "CRD_HESAP_MICROKERNEL_BACKEND must be _INTRINSICS or _ASM");

#if CRD_HESAP_MICROKERNEL_BACKEND == CRD_HESAP_MICROKERNEL_BACKEND_ASM
// Reserved: a future v0d-asm-microkernel slice will define these extern
// symbols in engine/hesap-dense/src/asm/<arch>.S files and the dispatcher
// in gemm_microkernel.hpp will route to them when this branch is active.
// Today: intentionally a static_assert so accidentally enabling the asm
// backend without the per-arch files in place fails at compile time.
static_assert(false,
    "CRD_HESAP_MICROKERNEL_BACKEND_ASM is reserved-but-unimplemented. "
    "Per ADR-0082, asm microkernels ship only after the three-condition "
    "revisit gate is satisfied. Stay on INTRINSICS unless you have just "
    "landed engine/hesap-dense/src/asm/<arch>.S files.");
#endif
