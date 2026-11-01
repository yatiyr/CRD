# 2026-11-01 — SIMD-PERF: the f32 1K and 8K forwards against MKL, profiled (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-1k-8k-profile.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3. One start, 0.28 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO): HEAD `c93abf76`
  (`~/ab/b12-candB`, byte-identical to a fresh HEAD build) and the candidate (`~/ab/b13-cand`): `in-place-3` offered
  at 1K (n = 16·4·16, a generated 4-point pass 2). The candidate changes no other forward, so its standalone-hier,
  MKL and 8K arms are HEAD's code. `CRD_CPU_LEVEL=avx512` and `avx2` select the fft units.
- **Peer:** Intel MKL 2020.0.4 in the same process, its AVX-512 library at both levels.
- **Method:** the profile loop (`bench_fft_f32 --loop`, 400,000 transforms at 1K and 50,000 at 8K, each from a
  fresh copy of the input) through `scripts/prof_fft_f32.sh` with `uops_dispatched.port_*`,
  `mem_inst_retired.all_loads`/`all_stores` and `ld_blocks.*` counted; counts are per transform, the copy side's
  subtracted where a row says net. The 1K arms board through `scripts/ab_fft_f32.sh` with `CRD_AB_ARMS` (each timed
  transform warm, best of 200 a process, one warm-up, median paired ratio [percentile-bootstrap 95%], 2% band); the
  8K arms from one process (best of 200). Instruction mixes from `objdump` of the hot codelets.

## 1K at avx512: the bound

Per transform, with the copy (the copy alone: 279 cycles, 137 instructions, 47 port-5 operations):

| side | cycles | instructions | port 0 | port 5 | loads | stores | hottest symbols |
|---|---|---|---|---|---|---|---|
| standalone-hier (planned) | 2,593 | 6,257 | 1,383 | 2,197 | 1,147 | 904 | stage 1 `codelet32_stage1_fused_sh_cs` 55%, `codelet32_batched_sc` 29% |
| MKL | 1,850 | 4,658 | 1,248 | 1,599 | 837 | 464 | `rowbatch_twidl_fwd_064` 65%, `colbatch_plain_fwd_16` 18% |
| in-place-3 (candidate) | 2,386 | 6,515 | 1,481 | 2,057 | 1,819 | 1,085 | pass 1 33%, pass 3 23%, pass 2 18%, group transpose 13% |

Both the planned forward and MKL are core bound (42% and 45% of slots) on port 5 (93% and 99% busy net of the copy:
MKL saturates it with 28% fewer operations); port 1 takes no vector operation while 512-bit ones are in flight. The planned 1K is 32 x 32 in two steps a
pass, and each step of stage 1 carries 448 transposing operations (128 `vshufps`, 128 `vunpck*ps`, 128
`vshuff32x4`, 64 `vextractf32x8`: the transposed store into AoSoA rows) beside its 64 `vpermt2ps` deinterleave, and
each step of the final leaf 64 `vinsertf32x8` (a two-tile block row loaded into one `Vec16f`) beside its 64
`vpermt2ps` interleave: about 1,280 shuffles a transform against about 2,040 vector FP operations. Stage 1 also
spills (128 stack stores and 155 stack loads a step), which is the 1.9x MKL's stores. MKL runs 64 x 16.

## The candidate: in-place-3 at 1K, measured and not kept

The in-place passes cut the transposing shuffles to the four group transposes (96 each) and ran 8% fewer cycles in
the loop, but its clock was lower: whole-process cycles over task-clock in two alternating rounds gave 2.93 and 2.96
GHz for in-place-3 against 3.18 and 3.20 for the standalone-hier (the copy alone 3.09), so the two ran level in time
(0.773 to 0.809 against 0.772 to 0.781 microseconds a transform with the copy). On the warm board it lost:

| level | n | standalone-hier ms | in-place-3 ms | in-place-3 speed ratio | MKL against standalone-hier |
|---|---|---|---|---|---|
| avx512 | 1K | 0.00071 | 0.00086 | 0.835 [0.814-0.837] slower | 1.183 [1.164-1.200] |
| avx2 | 1K | 0.00096 | 0.00122 | 0.787 [0.787-0.793] slower | 1.574 [1.548-1.574] (its AVX-512 library) |

Five repetitions at each level, every cell decided. Pass 2 at B = 4 is a full read and write of the buffer for a
4-point leaf (the loads 1.6x the standalone-hier's), and a single transform pays the three passes' and four group
transposes' store-to-load chains that a stream of transforms overlaps. Not kept: the generator, kernel and planner
were reverted.

## 8K at avx512: the profile and every arm

The loop, per transform with the copy (the copy alone 5,866 cycles at 8K, its 64 KiB larger than the L1d):

| side | cycles | instructions | port 0 | port 5 | loads | stores | hottest symbols |
|---|---|---|---|---|---|---|---|
| standalone-hier (planned) | 47,200 | 60,253 | 14,078 | 20,875 | 12,909 | 10,129 | `codelet128_batched_sc` 51%, `codelet64_stage1_fused_sh_cs` 37% |
| in-place-3 | 53,504 | 58,245 | 14,654 | 20,001 | 16,244 | 9,640 | pass 1 31%, group transpose 24%, pass 2 15%, pass 3 10% |
| MKL | 40,862 | 45,187 | 13,796 | 17,843 | 8,165 | 3,723 | `rowbatch_twidl_fwd_032` 51%, `colbatch_twidl_fwd_16` 19%, `colbatch_plain_fwd_16` 15% |

The planned 8K stores 2.7x MKL's: its 128-point final leaf holds 128 complex values in 32 registers and spills. MKL
runs three passes of 32, 16 and 16. Every arm, best of 200 in one process:

| level | standalone-hier | in-place-3 | deep-split-3 | deep-split-4 | ip4 | MKL |
|---|---|---|---|---|---|---|
| avx512 | 0.01032 (0.84x MKL) | 0.00949 (0.91x) | 0.01111 (0.78x) | 0.01204 (0.72x) | 0.01164 (0.74x) | 0.00866 |
| avx2 | 0.01431 (0.56x) | 0.01300 (0.61x) | 0.01247 (0.64x) | 0.01253 (0.63x) | 0.01338 (0.59x) | 0.00795 |

The board's best single transform ranks in-place-3 first at 8K, the loop's mean the standalone-hier (15.0 against
16.9 microseconds a transform with the copy): at 1K and at 8K the two measures order the arms oppositely, so a
plan change at either size waits for one measure that settles both (the loop's per-transform distribution beside
its mean, and the clock of each arm).
