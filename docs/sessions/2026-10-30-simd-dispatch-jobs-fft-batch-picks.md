# 2026-10-30 — SIMD-DISPATCH: the batched transforms' crd-jobs picks and the default fiber tiers

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (Left item 9, the crd-jobs retune folded in from
[HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune)), its last consumer. Before:
[dense QR's and trsm's levers](2026-10-30-simd-dispatch-jobs-qr-trsm-picks.md).

## CI first

- Push run 40976392370 (c28aaa54), still running: its eight failed linux-gcc and linux-clang-tsan jobs all stop at the
  test_cholesky.cpp:340 double promotion fixed at bc2bdb83. Every completed run since was cancelled before a job
  started; 40993784639 (87695de9) is pending. Nothing cancelled or dispatched.
- Needs CI audit: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none has run. Nothing flips.

## What landed

- **The batches** (`fft/detail/batch_arms.hpp`): `detail::BatchPick`'s `min_transforms` (4, today's rule),
  `min_work` (transforms x n x ceil(log2 n), off by default) and `workers`; `batch_jobs` (1 on a pool that is down,
  below a cut or for one job, at most one job a transform) and `run_batch` (one contiguous range a job, job j on plan
  and scratch j). hesap-fft links crd-jobs. The consumers, each through a detail arm with production at the default:
  `segment_periodograms_arm` (Welch, the spectrogram), `stft_arm`, `cwt_arm` (their three copies of the split are
  gone) and `NdFftPlan::execute_arm`: a plan built for `job_plans` sets runs an axis's lines as a batch, a set a job;
  the default single set keeps today's serial transform. The inverse's conjugations stay serial.
- **Not a lever:** `FftPlan::execute_batched` (element-major). Its engine caller is the four-step inside one
  transform, at exactly the sub-plan's block width (the hierarchical route, whose stage-1 strides are the batch); no
  engine batch of independent transforms reaches it.
- **Defect fixed** (found by the arm tests): `jobs::Config`'s Large tier held 16 fibers against a 32-thread pool. A
  thread holds at most one fiber of a job that does not wait (the one it runs or keeps), so one Large-stack job a
  worker (Welch's segments on this host) could find the tier empty, an always-on fatal; a cap at the tier's count
  still raced the kept fibers (8 threads, 2 Large fibers: fatal on the second axis). The tier counts now default to 0,
  the base count (128, 64, 16) raised to `num_threads`; explicit counts are kept as given.

Determinism: every lever is scheduling. Each transform runs on an identical plan and writes only its own output.

- **Tests:** `[batch-jobs]` (fft): the cuts and forced jobs, run_batch's ranges, NdFftPlan (f64, f32) on pow-2,
  Bluestein and degenerate axes, fewer lines than jobs, forward and inverse, IEEE specials and guard words, at 2 to 32
  job sets and the cuts at 0, 4 and never, against one job bit for bit (10 s on win-debug); twenty workers on the
  default tiers, rounds of a Large job a worker with no refusal. dsp: the segment periodograms and stft, cwt: the
  scales, each at 0 to 32 forced jobs and the three cuts on eight workers against one worker. jobs: the default
  tiers at 8, 20 and 70 threads.
- **Board:** `bench_hesap_fft_parallel` (`--threads N`, production's jobs configuration): Welch f64 1M samples at
  nperseg 256 and 4096, stft f32 at 64 and 1024, cwt f64 n 1K and 16K with 64 scales, NdFftPlan f64 512^2, 64^3,
  1000 x 768 and f32 1024^2; arms production, every batch, the work cut from 256K and from 4M; each gated bit for bit
  against one job, a miss exits 1. `scripts/run_bench_fft_parallel.sh` runs it at 4, 8, 16 and 32 threads.

## Checks

win-debug builds of the fft, dsp, wavelet and jobs tests and the bench; the new cases pass, with the existing
`[spectral]`, `[stft]`, `[cwt]`, `[diag10c]` and `[fiber_keep]` cases; strict tidy (LLVM 20) clean on the eleven
changed C++ files; clang-format on the added lines; Allman braces; the ctest guards (19 pass serially). Two `-j4`
guard runs each failed once with an impossible Python error (a `list_iterator` as a symbol name in
check-isa-isolation, "internal error in regular expression engine" in check-allocation-results); neither reproduced
in three reruns: host misexecution under load, as recorded since 2026-10-11, not a code defect. No bench ran: the
boards run at row close.

## Left

The retune's levers are all written; the boards per level with the thread sweep remain.
