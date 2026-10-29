// The sparse direct factors' crd-jobs levers (SIMD-DISPATCH, the folded HESAP-JOBS-RETUNE): each factor's production
// entry runs its arm on the default pick, so production and the arms share one body; the thread-sweep board
// (bench_hesap_direct_parallel) sets the defaults, and a lever that loses goes with its field.
//
// Determinism (ADR-0063): a scheduling lever decides only which job computes an entry, never its reduction, so every
// setting gives the one-job result's bits. A blocking lever regroups sums: its arms are held to the factor's
// backward-error bound, and to their own one-job bits at every job count.

#pragma once

#include <crd/core/types.hpp>

namespace crd::hesap::direct::detail
{

// SupernodalCholesky's factorize_arm (the first six) and solve_arm (the last four).
struct SupernodalPick
{
    crd::u32 node_min_cols = 512;          // scheduling: a thin etree level runs node-parallel with a front this wide
    crd::u64 gemm_min_flop = 1ULL << 20;   // scheduling: a node-parallel front's gemms and subtracts split from here
    crd::u32 cmod_syrk_min = 48;           // scheduling: f64 on one job per front, a descendant's m1 x m1 is a syrk
    crd::u32 cdiv_scalar_max = 32;         // blocking: fronts this narrow take the scalar rank-1 cdiv
    crd::u32 cdiv_block = 256;             // blocking: the blocked cdiv's outer panel, the trailing update's depth
    crd::u32 cdiv_inner = 64;              // blocking: its inner panel, 1 to cdiv_block
    crd::u64 solve_min_work = 160'000'000; // scheduling: the multi-RHS solve runs level-parallel from this lnz nrhs
    crd::u32 solve_max_workers = 8;        // scheduling: on at most this many jobs
    crd::u32 solve_workers = 0;            // scheduling: 0 takes the two gates above, otherwise the forced jobs
    crd::u32 solve_batch_min_nc = 48;      // blocking: the multi-RHS diagonal solve batches from this front width
};

// SupernodalLU's factorize_arm; the job count stays factorize's parameter.
struct SupernodalLuPick
{
    crd::u64 cmod_gemm_min_flop = 1ULL << 18; // blocking: a contributor's foot update below this is the inline loop
    crd::u32 diag_block = 48;                 // blocking: the diagonal block's LU panel, its Schur gemm's depth
};

// factor_front's (dense_lu_kernels.hpp) within-front levers. They act only on a front factored on the main thread
// (factor_front's gemm_par), never inside a parallel walk over fronts, so no setting nests a parallel_for.
struct DenseLuPick
{
    crd::u32 panel = 128;                   // blocking: the right-looking LU's panel width, the Schur gemm's depth
    crd::u32 trsm_min = 256;                // scheduling: U12 = L11^-1 A12 splits its columns from this many on
    crd::u64 schur_min_mnk = 256ULL * 1024; // scheduling: the trailing Schur gemm runs on the jobs from this m n k
    bool lookahead = true;                  // scheduling: the next panel factors on one job beside that gemm
    crd::u32 workers = 0;                   // scheduling: 0 is crd::jobs::num_workers(), otherwise the forced jobs
};

// MultifrontalLU's factorize_arm; the walk's job count stays factorize's parameter, the fronts' is front.workers.
struct MultifrontalLuPick
{
    crd::u64 small_fill = 1ULL << 20;  // scheduling: a factor with less L + U fill walks its fronts serially
    crd::u32 front_par_min = 0;        // scheduling: a level of this many fronts runs front-parallel, a narrower
                                       // one front by front on the main thread; 0 is half the pool, at least 2
    crd::u32 zero_fill_min = 1U << 18; // scheduling: a main-thread front this large clears on the jobs
    crd::u32 extend_add_min = 256;     // scheduling: a child's contribution this wide scatters on the jobs
    DenseLuPick front = {};            // the fronts' factor_front
};

} // namespace crd::hesap::direct::detail
