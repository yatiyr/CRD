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

// A front's within-front jobs: its gemms of at least gemm_min_mnk m n k split across the jobs, which give gemm's bits.
struct FrontJobsPick
{
    crd::u64 gemm_min_mnk = 256ULL * 1024ULL; // scheduling
    crd::u32 workers = 0;                     // scheduling: the jobs; 0 is crd::jobs::num_workers()
};

// The level walks of MultifrontalLDLT, MultifrontalQR and MultifrontalCholeskyBlr. A level of at least level_min
// fronts runs front-parallel, each front on one job; a narrower level runs front by front on the main thread, where a
// front takes `front`'s jobs. A front-parallel level's fronts stay on their job, so no setting nests a parallel_for.
struct FrontWalkPick
{
    crd::u32 level_min = 1;   // scheduling: 1 runs every level front-parallel
    FrontJobsPick front = {}; // a main-thread front's
};

// MultifrontalLDLT's factorize_arm; the walk's job count stays factorize's parameter. The blocked Bunch-Kaufman is
// real-only: complex fronts take the unblocked kernel at every setting.
struct MultifrontalLdltPick
{
    crd::u32 block_min = 128;   // blocking: a real front with this many fully-summed columns takes the blocked kernel
    crd::u32 block_panel = 128; // blocking: its panel width, at least 2 (a 2 x 2 pivot fits in one panel)
    crd::u32 flush_tile = 192;  // scheduling: its trailing flush runs as gemms of this many lower columns
    FrontWalkPick walk = {};    // a main-thread front's flush gemms
};

// MultifrontalQR's factorize_arm; the walk's job count stays factorize's parameter. The compact-WY path is real-only.
struct MultifrontalQrPick
{
    crd::u32 panel = 48;     // blocking: the compact-WY panel width
    crd::u32 block_min = 64; // blocking: a real front with this many rows takes the compact-WY path
    FrontWalkPick walk = {}; // a main-thread front's larfb gemms
};

// MultifrontalCholeskyBlr's factorize_arm; the walk's job count stays factorize's parameter. Unlike LDLT's and QR's,
// a main-thread front takes its jobs on the serial walk too. A level runs front-parallel only when every front of it
// is dense: a BLR front's compression reaches hesap-dense's own jobs.
struct MultifrontalBlrPick
{
    FrontWalkPick walk = {.level_min = ~crd::u32{0}}; // a BLR front's gemms; by default the serial walk
    crd::u64 dense_gemm_min_mnk = ~crd::u64{0}; // scheduling: a main-thread dense front's trsm and Schur gemms split
                                                // from this m n k on walk.front's jobs (its diagonal Cholesky
                                                // takes them at Cholesky's own cuts)
};

} // namespace crd::hesap::direct::detail
