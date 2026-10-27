// The table of the hesap-fft kernels (SIMD-DISPATCH, ADR-0134 §5): the whole transform of a plan, one table per CPU
// level the kernel unit (../../../../kernels/fft_kernels.cpp) is built for, resolved by fft_kernels() in
// crd/hesap/fft/fft_dispatch.hpp. The header holds only data declarations, so a kernel unit compiles nothing from it.
//
// FftPlan (crd/hesap/fft/fft.hpp) builds the plan in general code and hands the kernels its FftPlanState: the sizes
// the planner chose, its tables and its scratch. A kernel runs the transform at its own level, sub-plans included.
// Determinism (ADR-0134 §7): bit-exact, portable the reference. Every level runs one algorithm per size on SIMD types
// whose operations give the same bits on every backend (single-rounded fma, two-rounded products elsewhere), so a
// transform gives the same bits on every level, run and thread; only NaN payloads may differ.

#pragma once

#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>

namespace crd::hesap::fft
{

enum class FftDirection : crd::u8
{
    Forward = 0,
    Inverse = 1,
};

// One plan as the kernels see it. Every table and scratch buffer belongs to the FftPlan that filled this in; the
// scratch is written through a const state (one plan per thread).
template <typename T> struct FftPlanState
{
    crd::usize n = 0;
    crd::u32 log2 = 0U;
    crd::usize n1 = 0; // four-step split ≈ √n (n2 = n/n1)
    bool use_four_step = false;
    // standalone-hier (1024..64K, f64+f32): scratches + the full stage-twiddle table + the n1 split
    Complex<T>* sh_s = nullptr; // allocated only for the deep-split band (the 2-pass sh runs din→t→din)
    Complex<T>* sh_t = nullptr;
    T* sh_twr = nullptr; // full [k*n2+u] table, OR the factored LO table when sh_msh != 0
    T* sh_twi = nullptr;
    T* sh_hir = nullptr; // factored HI table (W_n^{k·M·uh}); null when the full table is used
    T* sh_hii = nullptr;
    crd::u32 sh_msh = 0;  // factored-twiddle shift (M = 1<<msh); 0 = full table
    crd::usize sh_n1 = 0; // sh: the stage-1 leaf; deep-split: A (the S1 leaf)
    // deep-split: n = A·B·C, three generated passes. 0 = not deep.
    crd::usize ds_b = 0; // B (the S2 leaf); C = ds_c (the S3 leaf); A = sh_n1
    crd::usize ds_c = 0;
    crd::u32 ds_ash = 0; // log2(A) — the S2 broadcast-twiddle lane-repeat shift
    T* ds_twr = nullptr; // compact twiddle of the LAST notr stage: W_{BC} (3-stage) / W_{B2C} (4-stage)
    T* ds_twi = nullptr;
    // 4-stage deep-split: n = A·B1·B2·C. 0 = 3-stage.
    crd::usize ds_b2 = 0; // B2; when set: S2 = notr B1 (W_{B1B2C} below), S3 = notr B2 per k1 block
    T* ds_tw2r = nullptr; // S2 twiddle W_{B1·B2·C}^{k·v}, [k*(B2*C) + v] (B1·B2·C entries)
    T* ds_tw2i = nullptr;
    // IP4-AoS: the interleaved in-place radix-4 engine's digit-reversal and per-pass DIT twiddles. 0 = off.
    crd::usize ip_n = 0;
    crd::usize* ip_rev = nullptr;     // base-4 digit-reversal permutation (slot of input j)
    T* ip_twr = nullptr;              // per-pass DIT twiddles, concatenated: per pass 3·q entries each
    T* ip_twi = nullptr;              // (w1[k],w2[k],w3[k] as three k-runs), vector-consumable
    const FftPlanState* p1 = nullptr; // four-step: the length-n1 sub-plan
    const FftPlanState* p2 = nullptr; // four-step: the length-n2 sub-plan
    Complex<T>* tbuf = nullptr;       // four-step: transpose intermediate, 64B-aligned (NT-store target)
    Complex<T>* scratch = nullptr;    // four-step: cache-resident block scratch, 64B-aligned
    const T* tw_re = nullptr;         // W_n^k, k < n
    const T* tw_im = nullptr;
    const crd::usize* rev = nullptr; // bit reversal of k < n
    T* re0 = nullptr;                // SoA ping-pong scratch, n each
    T* im0 = nullptr;
    T* re1 = nullptr;
    T* im1 = nullptr;
    // Four-step twiddle factorization: W_n^a = W_n^{a_hi·M}·W_n^{a_lo} from two ~√n L2-resident tables
    // (a_lo = a & (M−1), a_hi = a >> ftw_h, M = 1<<ftw_h).
    const T* ftw_hi_re = nullptr; // W_n^{j·M}, j ∈ [0, n/M)
    const T* ftw_hi_im = nullptr;
    const T* ftw_lo_re = nullptr; // W_n^{j},   j ∈ [0, M)
    const T* ftw_lo_im = nullptr;
    crd::u32 ftw_h = 0;
    const T* ptw_re = nullptr; // per-pass combine twiddles, flat in (j,m) pass order
    const T* ptw_im = nullptr;
    crd::u32 rmax_bits = 3U;         // size-aware max radix bits per pass
    Complex<T>* hier_bbuf = nullptr; // transposed intermediate of the hierarchical sub-FFT
    T* hier_twr = nullptr;           // its inner twiddle  cos(2π·n2·k1/n)
    T* hier_twi = nullptr;           // its inner twiddle −sin(2π·n2·k1/n)
};

// The twiddles of the f64 small-N lane-trick codelets (detail/small_n_codelets.hpp, N 8, 16 and 32), each entry the
// four lanes of a Vec4d. small_n_twiddles() builds them once in general code.
struct SmallNTwiddles
{
    alignas(32) crd::f64 w81r[4]; // size-8 split-radix leaf twiddles (W8^1, W8^3)
    alignas(32) crd::f64 w81i[4];
    alignas(32) crd::f64 w83r[4];
    alignas(32) crd::f64 w83i[4];
    alignas(32) crd::f64 w16r[4][4]; // size-16 split-radix leaf twiddles (W16^k, W16^3k)
    alignas(32) crd::f64 w16i[4][4];
    alignas(32) crd::f64 w163r[4][4];
    alignas(32) crd::f64 w163i[4][4];
    alignas(32) crd::f64 wp8r[2][4]; // N=8  combine pairs  [W8^{2j},  W8^{2j+1}]
    alignas(32) crd::f64 wp8i[2][4];
    alignas(32) crd::f64 wp16r[4][4]; // N=16 combine pairs  [W16^{2j}, W16^{2j+1}]
    alignas(32) crd::f64 wp16i[4][4];
    alignas(32) crd::f64 wp32r[8][4]; // N=32 combine pairs  [W32^{2j}, W32^{2j+1}]
    alignas(32) crd::f64 wp32i[8][4];
};

[[nodiscard]] const SmallNTwiddles& small_n_twiddles() noexcept;

// The kernels of one element type: the transform of a plan in place (data holds plan.n values), and `batch`
// independent transforms in element-major layout (element i of transform t at data[i * batch + t]).
template <typename T> struct FftTypeKernels
{
    void (*execute)(const FftPlanState<T>& plan, Complex<T>* data, FftDirection dir) noexcept;
    void (*execute_batched)(const FftPlanState<T>& plan, Complex<T>* data, crd::usize batch, FftDirection dir) noexcept;
};

struct FftKernels
{
    FftTypeKernels<crd::f64> f64_kernels;
    FftTypeKernels<crd::f32> f32_kernels;
};

// The levels with a body; the dispatcher refuses a build that compiles any other.
extern const FftKernels kFftKernelsPortable;
extern const FftKernels kFftKernelsSse2;
extern const FftKernels kFftKernelsAvx2;

#ifdef CRD_FFT_PROFILE
// Per-phase cycle counters of the kernels (scripts/prof_fft_kernel.sh builds them); src/fft.cpp defines and prints
// them.
namespace prof
{
extern unsigned long long g_first, g_combine, g_last;
extern long g_calls;
extern unsigned long long g_p1_gather, g_p1_sub, g_p1_tw, g_p2_gather, g_p2_sub, g_p2_scatter;
extern long g_fs_calls;
extern unsigned long long g_ds1, g_ds2, g_ds3;
extern long g_ds_calls;
extern unsigned long long g_ip_gather, g_ip_pass;
extern long g_ip_calls;
void dump() noexcept;
void dump_four_step() noexcept;
void dump_ip() noexcept;
} // namespace prof
#endif

} // namespace crd::hesap::fft
