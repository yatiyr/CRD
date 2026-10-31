# 2026-10-31 — SIMD-PERF: the benchmark machine's method, and the fft f32 forward profiled against MKL

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads). First batch of the
optimization track (user order 2026-10-31: a dedicated agent, the cloud machine started only when needed; fast
benchmarks that are enough for understanding and profiling). Board:
[2026-10-31-simd-perf-fft-f32-xeon](../bench/2026-10-31-simd-perf-fft-f32-xeon.md). Before:
[the close of SIMD-DISPATCH](2026-10-31-simd-dispatch-close-and-perf-track.md).

## CI first

The track's branch had no commits of its own before this session; nothing of it had reached CI.

## What landed

- **The lanes on a native checkout.** `scripts/wsl-lane.sh` builds from the checkout itself when it lives outside
  `/mnt` and no `CRD_MIRROR_SOURCE` is set (the mirror exists only for drvfs; `sync` and `check` do nothing there), so
  `run-lane-bench.sh` and every `run_bench_*.sh` driver run unchanged on the benchmark machine's `~/cerid`
  ([build-performance](../design/build-performance.md#wsl-source-mirror)).
- **`scripts/bench-host-setup.sh`**: the machine's idempotent setup (perf for the running kernel,
  `perf_event_paranoid` 1 kept in `/etc/sysctl.d`, the pinned Ninja and Vulkan headers as the hosted Linux lanes
  install them, FFTW 3 and MKL from the system) and its census.
- **`scripts/build-prerow.sh`**: the pre-row engine `2f2aed72` exported from history and configured once with its own
  linux-gcc-release preset and retained benches under `~/prerow`; each call builds named targets incrementally.
- **`scripts/ab-board.py`** (the fast A/B board, [method](../bench/README.md#fast-ab-boards)): commands run
  interleaved on one pinned CPU in a rotated order after discarded warm-ups; arms are regular expressions over a
  command's output (two arms may read one process, so Cerid and its peer timed together pair exactly); each cell is
  the median per-repetition speed ratio with a percentile-bootstrap 95% interval (diag-overhead.py's statistics,
  imported, not copied) judged against a noise band (faster, slower, level, unresolved), and the board stops once no
  cell is unresolved. An instrument failure (a failed process, a missing or repeated key) stops it. Six cases in
  `test-repository-tools.py` (`AbBoard`).
- **The fft drivers:** `scripts/ab_fft_f32.sh` (the level's planned f32 forward against MKL, with
  `CRD_AB_CANDIDATE` a candidate build as a third arm and HEAD the reference), `scripts/ab_fft_prerow.sh`
  (`prof_fft_kernel` of the pre-row engine and of HEAD at each level), both recording `build-bench.py host` before
  and after, and `scripts/prof_fft_f32.sh` (perf stat top-down levels 1 and 2 with cycles, instructions and branch
  misses, then perf record's hottest symbols, one side per process).
- **`bench_fft_f32`**: `--sizes A-B` and `--planned` (the level's planned forward beside MKL only: the board's fast
  arms), and `--loop <side>` (one side's forward K times at one size, each from the input copy as in the board;
  `copy` alone is the control); its time columns print five decimals (the three-decimal milliseconds left 1K to 32K
  unresolved).
- **The pocketfft pin.** `cmake/pins.json` pins mreineck/pocketfft at `4d865cba` (the cpp branch's head; the project
  tags no release; BSD-3-Clause, LICENSE.md read from the archive), fetched by `CRD_BUILD_HESAP_VS_FFT_REFS` like
  HPTT; `CRD_POCKETFFT_DIR` and the unversioned `~/fft_refs` copy are gone, and the peer's version line names the
  commit. The license manifest, pinned-inputs.md, the source routes and the PinnedInputs case name it. FFTW and MKL
  stay system packages of the measuring host (their versions in the board's census).

## The machine

GCE spot `c4-standard-8`, Xeon Platinum 8581C (Emerald Rapids), 4 cores of 2 threads, L2 2 MiB a core, L3 260 MiB
shared with other tenants, real AVX-512 (and AMX). It ran 0.76 h in four starts: setup, the HEAD tree (188 steps),
the pre-row tree (190 steps), the boards and the profiles; the huge-page experiment; the verification run; tidy and
the rebased build. Stopped after each.

## Profile summary (the bound, with its evidence)

- **1K to 64K, the kernel.** At 4096 the planned standalone-hier executes 1.28x MKL's instructions at 0.74x its IPC
  (net of the input copy). Its code comes from the legacy decoder (MITE 2.44G cycles against 2.7M from the uop cache;
  MKL 15M against 0.98G) and icache misses stall 6.2%: the two whole radix-64 codelets exceed the uop cache. Backend:
  store bound 19.8%, L1 bound 14.6%, L2 bound 10.3%; the stage-1 pass streams the input, the full stage-twiddle table
  and the AoSoA scratch, about 96 KiB against the 48 KiB L1d (the factored-twiddle stage 1 is used from n2 = 1024;
  `build_stage1_twiddles` records it 3% to 6% slower at n2 <= 256 on the i9, so it is not the lever here). MKL runs
  three smaller passes (a 32-point row batch with twiddles, then 16- and 8-point column batches) from the uop cache.
- **2K, 2M and 4M, the plan.** Another offered forward beats the planned one on this machine: standalone-hier at 2K
  (0.85x to 0.89x MKL against ip4's 0.74x to 0.80x), deep-split-4 at 2M (1.43x to 1.53x MKL against the four-step's
  0.85x to 1.07x) and six-step at 4M (1.44x to 1.55x against 0.84x to 0.97x). The four-step at 2M takes 63M cycles a
  transform against deep-split-4's 35M and MKL's 44M: 71% memory bound, half its samples in its own body's memory
  passes; not the TLB (huge pages remove its page walks, 6.4% of cycles, and leave its time unchanged). On the i9 the
  four-step wins these sizes by 1.3x to 1.4x (board 3's data), so the best plan depends on the machine's memory
  system, not on the level: neither the level tables nor the canonical planner can hold it as written.
- **f64 from 2M, the same plan at a larger cost.** One run of `bench_fft_vs_refs` puts the planned f64 forward at
  0.32x to 0.36x of FFTW from 2M to 8M (0.37x to 0.60x of MKL), the widest gap measured; its own arm rows rank the
  six-step first at 4M where the canonical planner runs the four-step.
- **128K to 1M** Cerid leads MKL 1.17x to 1.70x at both levels; against the pre-row engine HEAD is level or faster at
  every measured size (f64 `prof_fft_kernel`: avx2 0.99x to 1.08x, avx512 1.15x to 1.45x).

## Decisions

- No engine change this batch: the profile names two levers, and neither is a one-line change that the rule (a
  decisive measured win, bit-exact or within the declared class) admits without its own board. Worst gap first: the
  four-step from 2M (f64 0.32x to 0.36x of FFTW, f32 0.70x of MKL), whose own body's memory passes are the bound
  here, then the 2K to 64K kernel (0.44x to 0.52x of MKL at 2K).
- The avx512 fft plan table is QUAL.1-HW's clause (per-level boards at avx512 on AMD and Intel CPUs); this Xeon's
  avx512 rows are recorded as evidence there, not generated into `level_plans_gen.hpp`.
- Huge pages were tried only as a diagnostic (set to `always` and restored to `madvise`); they are not a lever for
  the four-step.

## Checks

On the machine: the lane tree configured from the pinned pocketfft archive and built `bench_fft_f32`,
`bench_fft_vs_refs`, `prof_fft_kernel` and `cpu_level_record` without a warning; `bench_fft_vs_refs` printed the
pinned commit and passed its gates (exit 0); `bench_fft_f32`'s new options and its usage error (exit 2) ran; a
smoke board through `ab_fft_f32.sh` wrote the host snapshots (idle); the repository tooling suite on Linux passed
every case but the generated-sources record of four fft codelet headers, stale on main since `4859c057` and refreshed
there before this delivery (the Ninja-gated IDE model case included, which this Windows shell cannot run without
Ninja). LLVM-20 tidy (`tidy-files.py`, the pinned clang-tidy 20.1.0, the lane tree's database) on the two changed
units: `bench_fft_f32.cpp` drew `readability-use-anyofallof` on the side lookup, now `std::ranges::find_if`, then both
clean; the rebuilt `--loop deep-split-4` and the unknown-side error (exit 2) ran again. Locally after the rebase: the `AbBoard`, `Provenance` and `PinnedInputs` cases, check-pins, the license
manifest and generated-sources checks, the Allman braces, the plan and repository checks.

## Found in the machine's driver (reported, outside this track's files)

`ci/bench_vm.py`, which this track uses but does not own:

- Since the `--name` option, `wait NAME` fails at once ("--name must start with cerid-bench or cerid-track-"): the
  `wait` subcommand's positional `name` writes the same argparse destination as the top-level `--name`. This session
  polled `~/jobs/<name>.exit` through `run` instead.
- `sync` after a rewritten history (a rebase or an amend of a commit already synced) fails: the bundle's fetch updates
  `refs/bench/sync` without `+`, a non-fast-forward the remote git refuses (the error surfaces only as plink's exit 1).
  Deleting the remote ref first (`git -C ~/cerid update-ref -d refs/bench/sync`) works around it.

## Left

The four-step from 2M (f64 and f32) on this machine's memory system, then the 2K to 64K kernel against MKL, then
the plan choice where the winner depends on the machine (2K, 2M, 4M); the cnn rows against pre-row (the next Left
item); each profiled and boarded on this machine first.
