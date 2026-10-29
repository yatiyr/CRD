# 2026-10-29 — SIMD-DISPATCH: the SELL job cut at a CPU's L2 share

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left items 3 and 4, the SELL policy and Delta16 boards).
Before: [the pick arms](2026-10-29-simd-dispatch-sell-pick-arms.md).

## CI first

- Scheduled run 40849112570 (fea7ee0a), linux-gcc-asan (job 123363959350, completed 14:59): crd-generated-sources and
  crd-repository-tools failed on the transcendental reference's provenance listing, the every-lane pair fixed at
  a721d147 and d334c12f (both pass at e496d16c on every lane that finished), plus TP-5 (mikktspace's registered shift).
  Nothing new. linux-gcc-arm64-asan and win-asan still run; nothing was cancelled.
- Push run 40869334591 (e496d16c): linux-clang-arm64-asan passed 2476 of 2476 with the forced portable and neon arms
  of crd-math, crd-geometry-primitives and crd-hesap-fft. Of the ARM64 lanes only linux-gcc-arm64-asan is left.
- The ISA guard passed on seven more lanes: 94 s (linux-clang-arm64-asan), 154 s (win-clang-cl), 173 s
  (linux-gcc-debug-scalar), 200 s (win-public-checks), 551 s (win-shipping, whose job failed on the TickClock case
  fixed at d3b0daf1), 567 s (win-clang-cl-shipping) and 929 s (win-relwithdebinfo, the slowest so far, about half
  the 1800 s timeout).
- No complete-tier run was dispatched: the scheduled run is still in progress. DIAG.11c and DIAG.12 stay Needs CI.

## What landed

- **`CpuSelection::l2_share_bytes`** (`crd/core/cpu_features.hpp`): the smallest L2 instance's bytes per logical CPU
  sharing it, read once by `detect_cpus()` from the OS on every architecture: Windows `RelationCache` group masks,
  Linux `cache/index*/shared_cpu_map` counted as it streams. CPUID's sharing word cannot give it. The sharers are the
  hardware's, not the process's affinity: a sibling thread uses the cache whoever runs it. No heap: the Windows
  buffer is pages (`VirtualAlloc`), the Linux reads are fixed chunks; the mask bits are counted by a loop, since
  MSVC's `std::popcount` is POPCNT behind a CRT branch, which the ISA guard refuses. This host: 1 MiB (the P-cores'
  2 MiB over two threads, the E-core clusters' 4 MiB over four).
- **`CpuSelection::os_l2_bytes`**: the smallest L2 instance the same reading sees. `sparse_resident_bytes()` takes
  CPUID's L2, else this, else 2 MiB, so on ARM64 (no CPUID caches) the whole-L2 cut, the Auto index and the
  operators' threshold use the measured L2 rather than a 2 MiB guess beside a measured share.
- **`sparse_job_resident_bytes()`**: the L2 share, the whole L2 where the OS describes none.
- **`SellJobCut { WholeL2, CoreShare }`** and `detail::spmv_sell_parallel(..., cut)`: each job's range picks its
  sweep against the whole L2 (production, unchanged) or its share. `detail::sell_spmv_range` takes the cut as
  `resident_bytes`; `spmv_sell` passes the whole L2.
- The Delta16 Auto cut needs no code: the board's arms are the forced `SellIndex::U32` and `Delta16` builds at sizes
  between the L2 and the L3; the cut then moves to where Delta16 starts to win.

Every cut gives the same bits (a sweep only picks among bit-exact bodies).

## Checks

- crd-core-tests and crd-hesap-sparse-tests built on win-debug. `[cpu]` passes, with the share equal to an
  independent oracle that reads Windows' masks bit by bit and Linux's `shared_cpu_list` form. `[parallel]` and
  `[sell]` pass: a new case sizes a tridiagonal matrix between the share and the L2, so the two cuts sweep it
  differently, at 1, 3 and 7 jobs, U32 and Delta16, bit-exact with serial; the complex case runs both cuts.
- Strict tidy is clean on the nine changed C++ files; clang-format adds no finding outside the files' existing
  aligned groups; Allman braces, the plan and repository checks pass.

## Left

- The one-binary SELL board driver with the complex Eigen peer (forced picks, both job cuts at 1, 4, 8, 16 and 32
  jobs, U32 and Delta16 at sizes between the L2 and the L3, gated bit for bit against the CSR spmv), then the board
  in the row-close pass.
