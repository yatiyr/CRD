# 2026-10-31 — SIMD-PERF: the four-step's twiddled columns as whole cache lines

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), the first
session's next target: the four-step from 2M on the benchmark machine. Board:
[2026-10-31-simd-perf-fft-four-step-lines](../bench/2026-10-31-simd-perf-fft-four-step-lines.md). Before:
[the benchmark machine and the fft f32 profile](2026-10-31-simd-perf-bench-machine-fft-f32-profile.md).

## CI first

The first session's push run (41209376815, `3e596ed5`) on the mirror was cancelled before any job started,
superseded by the serial track's later pushes; no failure of this track's commits to fix. Own-CI builds now start
only on the user's decision (`45a9c03e`), so none ran for them.

## Profile (the bound, with its evidence)

The first session's profile had put half the 2M four-step's samples in `execute_four_step`'s own body and called it
memory passes. The phase counters name the pass: **phase 1's twiddle-and-store step** takes 50% (f32 2M) to 69% (f64
2M) of the transform at avx512 and 17% to 21% at avx2, 3.2x (f32) and 8.9x (f64) more cycles at avx512 than at avx2
in the same body, which is why the avx512 level ran the f32 four-step 35% slower than avx2 and the f64 one 2.3x
slower. The step stored each column's one complex a k1 into that column's tbuf row: `lanes` rows in flight (sixteen
f32 or eight f64 rows at avx512's two-tile step), each n1 complex apart, all in one L1 set. f64's 16-byte
non-temporal stores held the fill buffers with partial lines (`l1d_pend_miss.fb_full` 68% of the cycles at avx512
against 23% at avx2, store-buffer stalls 3.2x); f32's plain 8-byte stores evicted their lines from the 12-way set
before the next store (L2 read-for-ownership requests 1.8x avx2's, store-buffer stalls 1.6x). Counts and phase table
in the board.

## The change

On the levels whose tile is one register (avx2) or two tiles (avx512), `execute_four_step`'s phase-1 step keeps
`lanes` consecutive k1 of its tile in registers, transposes the real and imaginary planes (`transpose_rows`, the
six-step's register transpose) and writes each column's run of k1 as whole 64-byte lines through `store_tile_row`
(f64 non-temporal, f32 plain: the stores each type had). The levels whose tile spans two narrower registers (sse4.2,
sse2, portable, NEON) keep the element stores (`kWholeLineTiles`). The arithmetic, its order and the twiddle
recurrence are unchanged, so the results are the same bits; the bw tail keeps the element path. The four-step is the
planned inverse from 128K and the planned forward from 2M (and every level's plan of its sizes), so both directions
take it.

How the level split was found: the first candidate took the line path at every level. Its boards were level at sse2
for f64 (0.987 to 1.000) but **3% slower for the f32 inverse at sse2** (0.967 to 0.973): there `Vec8f` is two
128-bit registers and a micro-tile of eight steps needs 32 of them in its two planes, twice the register file. A
second structure (the step's twiddle as a lambda shared by both paths) ran the f64 128K inverse 4.4% slower at avx2
than the first candidate's code on the same base (0.956 [0.955-0.965], interleaved three-build board, the same
cycles at a lower clock), so the kept change is the first candidate's loop with the store path selected inside it at
compile time, the same code at avx2 and avx512.

## Results (board)

HEAD and the kept candidate on one base, every board `--same` (the same digests, bit-exact), the speed ratio
[bootstrap 95%]:

- **avx512:** the f64 four-step 2.35x to 2.72x faster (2M forward 2.72, 128K inverse 2.59, 1M inverse 2.35; the
  first candidate's boards add 4M forward 2.51 and 2M inverse 2.37); the f32 transforms 1.48x to 1.73x (2M forward
  1.63, 4M forward 1.48, 128K inverse 1.73, 2M inverse 1.63); the f32 forward **ahead of MKL** at 2M and 4M (1.17x and
  1.12x; HEAD 0.72x and 0.76x).
- **avx2:** 1.01x to 1.06x (f64 2M forward 1.029, f32 forward 1.059 and 1.054; the inverses 1.013 to 1.024); the f32
  forward level with MKL (1.00x and 0.98x; HEAD 0.94x and 0.93x).
- **sse2:** level (0.991 to 1.000), the element stores HEAD had.
- **Against the pre-row engine** (the first candidate, the same code at these levels) the f64 forward leads by 1.21x
  and 1.23x at avx512 (HEAD had been at 0.44x and 0.48x there, sizes the first board's pre-row rows did not reach)
  and 1.04x and 1.06x at avx2.
- **Controls:** the six-step 1M forward (untouched) level at every level; the f32 1M tiled four-step level.

## After the change

The phase-1 step falls from 21.0 to 5.0 Mcyc (f32 2M) and from 83.1 to 7.9 (f64 2M) at avx512, and from 6.5 to 5.5
and 9.3 to 7.7 at avx2; `fb_full` at f64 2M avx512 from 3.65G to 0.65G cycles. At 2M the largest phase is now P2
(the fused 32x32 gather, its sub-FFT and the output scatter: 42% to 45% at both levels), at 4M the two sub-FFT passes;
the f32 2M forward at avx512 is 65.9% backend bound (49.9% memory). One run of `bench_fft_vs_refs` puts the f64
forward at 1.02x FFTW at 2M, 0.86x at 4M and 1.00x at 8M at avx512 (HEAD 0.32x to 0.36x), and its arm rows rank the
six-step ahead of the four-step at 4M at both levels.

## What landed besides

- **Bit digests on the boards.** `prof_fft_kernel` and `bench_fft_f32` print the FNV-1a digest of one transform's
  output (`bits`; the board rows and the `--loop` line); `ab-board.py --same NAME,NAME:REGEX` stops a board when two
  commands, or two repetitions of one, print different digests at a key, or a command prints none (a case in
  `test-repository-tools.py`, `AbBoard`).
- **The drivers:** `ab_fft_f32.sh` takes HEAD's saved build (`CRD_AB_HEAD`) and checks it against the candidate with
  `--same`; `ab_fft_prerow.sh` takes a saved build as the reference at each level (`CRD_AB_REFERENCE`) and sizes
  `N:K[:inverse][:f32]` (the inverse and the f32 transform, which `prof_fft_kernel` now runs), every size checked
  before the build; `prof_fft_f32.sh` counts further perf events (`CRD_PROF_EVENTS`) and profiles a saved build
  (`CRD_PROF_PROGRAM`).
- **The phase-counter tree builds again:** `CRD_HESAP_FFT_PROFILE` failed to compile `src/fft.cpp` under
  `-Wconversion -Werror` (implicit unsigned-to-double divisions in the dumps); one helper converts them, and both
  profile programs print the four-step's phases.
- **Two release links fixed** (found by the machine's whole-tree build with the fft peers on, which CI does not
  build): GCC's link-time pass failed `bench_dsp_conv_vs_refs` (a potential null dereference at the result's
  element: the bench now checks the convolution's length first) and `bench_tensor_io` (`npy_build_header`'s v2.0
  length went into four bytes unchecked, and a wrapped padding sum would have left an empty header that the writes
  dereferenced: the length is now checked against both). `io.hpp` and `bench_tensor_io.cpp` then pass LLVM-20 strict
  analysis (ten and five pre-existing findings).

## Checks

On the machine, the linux-gcc-release lane tree with every option it configures (the retained benches and the fft
peers included), on main's tree at `625bcb77` with the kept change: the whole tree built without a warning (2,501
steps on the first rebase, the two link failures above fixed); `crd-hesap-fft`, `crd-hesap-tensor` and every
`crd-no-*`, `crd-check*`, `crd-diag-coverage` and `crd-diag-seeded-faults` case passed, 512 with the per-level kernel
arms (portable, sse2, sse4.2, avx2, avx512), after the tensor fix and at the kept change (the first candidate's 90
fft cases before them); LLVM-20 tidy (`tidy-files.py`, the pinned clang-tidy 20.1.0, the lane tree's database) clean
on every changed C++ file; every HEAD-against-candidate board ran `--same`. After main moved FNV-1a onto crd-core
(`a89b18a4`) the programs take it from there; a second start rebuilt the two fft programs and the fft tests on the
delivered base (`0fc32e70`): the same digests as the boards, the 90 fft cases, tidy clean. The machine ran 2.14 h in
two starts.

Locally: `test-repository-tools.py`, 187 cases, all but two (neither from this session's changes): the generated
model case needs Ninja, which this shell lacks (as the first session noted), and
`test_every_registered_fuzz_target_has_a_driver_entry` fails on main since `5eea2fa9` registered
`crd-fuzz-net-negotiation` without a `scripts/fuzz.py` TARGETS entry (the NET track's area, reported); the
`AbBoard` cases, `ab_fft_prerow.sh`'s argument refusals (exit 2 before any build), the Allman braces, the
capability-owner guard (one host misexecution, rerun), the plan and repository checks.

## Found in the machine's driver

The first session's two `ci/bench_vm.py` defects were fixed by its owner during this session (`3cf62fa0`, `wait`;
`7459c35b`, `sync` through a forced ref; `3bb87c7f`, detached runs return). One remains: a `gcloud compute ssh` that
drops while the machine is under heavy build load surfaces as `wait`'s error and exit 143 instead of a retry
(three times, at load averages of 50 to 66); the job itself keeps running.

## Left

- The four-step's P2 at 2M: the fused 32x32 gather, its sub-FFT and the scatter take 42% to 45% of the 2M transform
  at avx512 and avx2 now (f32 12.0, f64 19.1 Mcyc at avx512); profile it against MKL's column passes.
- The f32 forward at avx2 sits at 0.97x of MKL at 2M and 4M.
- At 4M the program's arm rows rank the six-step ahead of the four-step at both levels (45.5 against 52.9 ms at
  avx512, 52.2 against 59.9 at avx2; single runs): board it, and if decisive it is a plan change, which the i9's
  boards must agree with (the machine-dependent plan item).
- The sse2 f64 forward: 78.1 ms at 1M (the sse2 plan's six-step) and 194.1 at 2M against 15.9 and 32.4 for the same
  level's inverse; at 2M the forward's sub-FFTs (P1's batched 2048-point plan 223.6 Mcyc against 25.5, P2's fused
  32x32 path 207.8 against 35.1).
- Then the 2K to 64K kernel (the first session), the cnn rows against pre-row and the jobs retune's boards.
