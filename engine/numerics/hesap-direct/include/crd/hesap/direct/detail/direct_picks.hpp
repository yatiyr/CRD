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

} // namespace crd::hesap::direct::detail
