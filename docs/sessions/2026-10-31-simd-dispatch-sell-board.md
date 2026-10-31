# 2026-10-31 — SIMD-DISPATCH: the SELL board on the busy host

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (row-close boards, the SELL family). Before:
[the CWT fatal specimen](2026-10-31-simd-dispatch-cwt-specimen-levels.md).

## User order (2026-10-31 ~00:10)

The user's words: "man we are looking for relationship between peers! go and do the fucking benchmarks! why the hell
you are not doing them! it does not matter whether the pc is busy or not!" The remaining row-close boards run now
without waiting for a quiet host: each pass records `build-bench.py host`; ratios to the peers and to the pre-row
engine are taken side by side (interleaved arms, alternating engines) with their spread, and a regression is real when
it holds across the repetitions. Order: SELL (five passes), permute against HPTT, the sse2/sse4.2 bodies, the retune's
thread sweep at 1, 4, 8, 16 and 32 threads.

## CI first

No completed run has a failed job: the cancelled push runs 41126520149, 41126260668 and 41120249137 hold none.
Push run 41112483075 (`37ebb440`, in progress, not cancelled) failed the repository job, win-debug-scalar,
linux-gcc-debug-scalar, win-clang-cl-shipping and win-arm64-debug: the CI platform store tests under the short
`TEMP` and the CWT specimen, both fixed on main. Its `MemoryError` lines are `ci/pipeline.py`'s intended cap case, not
a failure. Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables
`FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only); no secrets, so the AMD and Mac
lanes wait on their accounts.

## The board

[Record](../bench/2026-10-31-simd-dispatch-sell-board.md). Five passes in WSL (`~/sell-board/board2.sh`, each pass a
probe part and a threads part under one supervised hour; the parser prints per-pass ratios, their noise and a verdict
needing every pass), 5 h 50 min in all. Host: other Windows processes 0.7 to 2.5 cores, 10.9 to 16.6 GiB available.

- Complex and wide rows run faster than pre-row (c32 up to 3.9x); production leads Eigen at every type and level
  except portable f32 (0.946) and f64 uniform-4 (0.897 to 0.963 at avx2).
- The picks stay: production is within 4% of the best arm at half the L2 on every type and level. Beyond it the
  arm column carries an order bias (production pays first for its evicted matrix at half the L3).
- Six cells trail pre-row in every pass, within the contended noise: f64 uniform-4 (half L2, L2-L3), f64 power-law
  half L2, f32 uniform-4 (half L2, half L3, 1M).

## Attribution

The probe is two binaries, so `~/sell-board/sell_ab2.cpp` ran HEAD's production and 2f2aed72's loop on one matrix in
one process across eight layouts: f64 uniform-4 and banded-5 0.76 to 0.90 at every layout, f32 0.94 to 0.98 (the f32
probe gap was mostly layout), wide rows faster. In `sell_ab`, HEAD's own pick for these rows (scalar loads, one slice
alone, no prefetch) written as a flat loop runs 0.0204 to 0.0210 ms against production's 0.0255 to 0.0261: the
cost is the slice driver. Its avx2 f64 single-slice path reloads a 0x140-byte frame of spilled view fields and
stacks every slice's sums before the write, about 170 instructions a slice; the flat loop (from the linked `sell_ab`
binary) runs about 100, its frame fields in registers and its write `y[r] = acc` with no scale. Three experiments,
each built and run through `sell_ab2` at three layouts, were reverted:

- a local copy of the slice view (its fields no longer reachable through `a`): 0.83 to 0.89, unchanged, the fields
  still spilled;
- the pair width a constant (`kPairWidth`): 0.84 to 0.87, unchanged, so the pair test is not the cost;
- the lone slices' loop in a function of its own (`CRD_NOINLINE`): 0.77 to 0.80 and uniform-16 0.97 to 1.02, worse.

So the cost is spread over the per-slice bookkeeping (pair test, column base through `delta_ptr`, `slice_full`, the
stacked sums, the scaled write) under register pressure, not one item. The fix rebuilds the U32 lone-slice step as
the flat loop does (state hoisted once per range, the sums written from the register) and is the row's first item.

## Jobs

`spmv_sell_parallel` costs 0.33 to 0.35 ms at 4 to 16 jobs up to half the L3 on both engines, against Eigen's 0.006
to 0.2 ms; at 32 jobs 0.05 to 0.07 ms. That floor is crd-jobs' wake and join; the retune's thread sweep takes it
first, with the c32 power-law cell that lands in either mode.

## Left

The SELL driver's narrow-row cost fixed at its cause (the layout A/B and one avx2 board pass confirm it), then the
permute board against HPTT (the probe rebuilt from `~/permute-board/permute_probe.cpp`), the sse2/sse4.2 body boards
and the retune's thread sweep.
