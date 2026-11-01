# 2026-11-01 — SIMD-PERF: the f32 16K forward as the in-place three passes, and an aligned work buffer off a cache line (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-in-place-16k.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3. One start, 0.70 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO), saved as
  `~/ab/b15-head` (HEAD's fft engine, the [alignment board](2026-11-01-simd-perf-fft-alignment.md)'s), `~/ab/b15-cand`
  (in-place-3 offered at 16K, run in the caller's buffer at every address), `~/ab/b15-cand2` (and through an aligned
  work buffer off a 64-byte boundary at every size) and `~/ab/b15-cand3` (the work buffer from 8K only: the change).
  `CRD_CPU_LEVEL=avx512` and `avx2` select the fft units.
- **Peer:** Intel MKL 2020.0.4 in the same process, its AVX-512 library at both levels.
- **Method:** `prof_fft_f32.sh 14 50000` (each side's loop: top-down levels 1 and 2, the named events, `perf record`'s
  hottest symbols; counts per transform, the input copy included unless said); `ab_fft_f32.sh` with `CRD_AB_ARMS`
  (one build's forwards a process, each timed transform warm, best of 200, paired ratios over five to nine processes,
  percentile-bootstrap 95%, 2% band) and `CRD_AB_OFFSET` 0 (a cache-line-aligned buffer) and 16 (a malloc'd one);
  `ab-board.py` directly for two builds' in-place-3 rows under `--same` (their bits checked equal every repetition);
  `ab_fft_f32.sh` with `CRD_AB_CANDIDATE` for HEAD against the change where the plans agree.

## The 16K profile (avx512, aligned)

Per transform with the copy (the copy side alone 10,500 cycles):

| side | cycles | instructions | stores | loads | top-down | hottest symbols |
|---|---|---|---|---|---|---|
| ip4 (planned) | 89,300 | 167,700 | 27,600 | 39,500 | core 41.6%, memory 20.4% | `execute_ip4aos` 88% |
| standalone-hier | 93,200 | 130,600 | 23,000 | 28,300 | frontend 40.0% (MITE 132,000 of 133,400 uops) | `codelet128_stage1_fused_sh_cs` 50%, `codelet128_batched_sc` 39% |
| deep-split-3 (32·32·16) | 85,700 | 131,300 | 19,500 | 25,700 | memory 51.0% | `codelet32_fused_notr_ss` 48%, `codelet32_stage1_fused_sh_cs` 34% |
| deep-split-4 | 93,300 | 157,200 | 20,300 | 31,800 | memory 52.7% | four passes of 16 and 8 |
| four-step | 245,800 | 309,100 | 46,600 | 72,800 | memory 59.2% | copy 31%, `codelet128_batched` 31% |
| MKL | 62,800 | 98,800 | 8,200 | 18,700 | memory 41.4%, core 29.7% | `colbatch_twidl_fwd_32` 33%, `rowbatch_twidl_fwd_016` 28%, `colbatch_plain_fwd_32` 22% |

MKL runs 16K as three passes over the caller's buffer (leaves of 16, 32 and 32); every Cerid arm either runs seven
radix-4 passes (ip4: 1.7x MKL's instructions, core bound) or streams out-of-place intermediates (the deep-splits and
the standalone-hier, 2.4x to 2.8x MKL's stores). The lever is the shape: the in-place three passes, which ran only to
8K, at 16K = 16·64·16.

## In-place-3 at 16K, in the caller's buffer

`b15-cand`, the in-place-3 profile at each alignment against MKL's (per transform, the copy included):

| level, offset | side | cycles | instructions | stores | split loads | split stores | store-buffer stall cycles | hottest |
|---|---|---|---|---|---|---|---|---|
| avx512, 0 | in-place-3 | 73,200 | 124,300 | 21,400 | 0 | 0 | 34,900 | pass 1 38%, pass 2 21%, pass 3 14%, transpose 10% |
| avx512, 0 | MKL | 64,800 | 98,800 | 8,200 | 0 | 0 | 20,700 | |
| avx512, 16 | in-place-3 | 107,100 | 124,200 | 21,400 | 6,900 | 6,200 | 62,300 | pass 1 36%, transpose 24%, pass 2 16% |
| avx512, 16 | MKL | 87,700 | 98,800 | 8,200 | 3,100 | 2,100 | 32,000 | |
| avx2, 16 | in-place-3 | 144,800 | 270,500 | 51,500 | 8,100 | 6,200 | 68,500 | pass 1 33%, pass 3 22%, pass 2 21% |

`codelet64_inplace_mid` (pass 2's 64-point leaf) holds 64 complex values of 16 lanes in 32 registers: 293 stack
stores a step beside its 128 data stores (static count). On a buffer 16 bytes off a line, every pass's 64-byte block
straddles two lines: three passes of split loads and stores, where MKL's counts are one read and one write of the
buffer (3,100 split loads, 2,100 split stores).

Paired against ip4 (the planned forward), `CRD_AB_ARMS="ip4 in-place-3"`, `CRD_AB_REFERENCE=ip4`:

| level | offset 0 | offset 16 |
|---|---|---|
| avx512 | 1.308 [1.269-1.343] faster | 1.090 [1.072-1.126] faster |
| avx2 | 1.103 [1.078-1.108] faster | 0.809 [0.808-0.860] slower |

## The aligned work buffer off a cache line

From 8K the data leaves the L1d, and a buffer off a 64-byte boundary costs every pass its split accesses. The change
runs pass 1 out of place there (`codelet16_inplace_first_to`: the same 16-point leaf and twiddles, interleaved in
from the caller's buffer, AoSoA blocks out to the plan's aligned work buffer), passes 2 and 3 on the work buffer, and
pass 3's stores into the caller's: one read and one write of the caller's buffer, as MKL's counts show for MKL. The
operations and their order are the same, so the bits are the same at every address (the boards below check it every
repetition; a new test checks every 4-byte offset of a line at every level).

The work buffer against the caller's buffer, in-place-3's rows of `b15-cand` (reference) and `b15-cand3`, same bits:

| level | offset | 8K | 16K |
|---|---|---|---|
| avx512 | 16 | 1.184 [1.171-1.207] faster | 1.167 [1.099-1.213] faster |
| avx512 | 0 | 0.998 [0.984-1.024] | 1.006 [0.982-1.018] level |
| avx2 | 16 | 1.246 [1.218-1.264] faster | 1.165 [1.161-1.238] faster |
| avx2 | 0 | 0.995 [0.978-1.008] | 0.988 [0.972-1.008] |

At 4K the work buffer lost: `b15-cand2`'s planned in-place-3 at offset 16 was 0.679x [0.662-0.707] HEAD's at avx512
(2K 0.980x [0.960-1.007]): its 64 KiB beside the 32 KiB data leaves the L1d that holds the in-place passes. So the
work buffer runs from 8K (`kInPlace3WorkFrom`), and 2K and 4K run as before.

## The 16K and 8K arms with the change

`b15-cand2` (the 8K and 16K routes are `b15-cand3`'s), in-place-3 the reference; above 1 the arm is faster:

| level | offset | n | MKL | standalone-hier | ip4 |
|---|---|---|---|---|---|
| avx512 | 0 | 8K | 1.138 [1.113-1.175] | 0.940 [0.925-0.968] | 0.835 [0.828-0.845] |
| avx512 | 0 | 16K | 1.131 [1.121-1.154] | 0.820 [0.806-0.836] | 0.791 [0.781-0.812] |
| avx512 | 16 | 8K | 1.098 [1.073-1.122] | 0.994 [0.979-1.038] | 0.872 [0.854-0.892] |
| avx512 | 16 | 16K | 1.097 [1.057-1.109] | 0.872 [0.857-0.880] | 0.791 [0.785-0.795] |
| avx2 | 0 | 8K | 1.601 [1.568-1.608] | 0.940 [0.918-0.978] | 0.971 [0.965-0.975] |
| avx2 | 0 | 16K | 1.586 [1.565-1.601] | 0.736 [0.702-0.745] | 0.922 [0.905-0.935] |
| avx2 | 16 | 8K | 1.534 [1.512-1.546] | 1.043 [1.022-1.046] | 0.993 [0.984-1.004] |
| avx2 | 16 | 16K | 1.494 [1.411-1.509] | 0.797 [0.777-0.819] | 1.003 [0.994-1.006] |

- **16K is planned as in-place-3** (the canonical planner, which avx512 runs under Default and every level under the
  deterministic modes; avx2's Default row from the i9's board keeps IP4-AoS): 1.26x IP4-AoS at avx512 at both
  alignments, 1.08x at avx2 aligned and level off a line. Against MKL at avx512 it is **0.884x aligned and 0.912x
  misaligned**, from 0.686x and 0.744x (the [alignment board](2026-11-01-simd-perf-fft-alignment.md)).
- **8K stays the standalone-hier** this batch: in-place-3 is 1.06x it aligned at both levels and level misaligned at
  avx512, 0.96x misaligned at avx2 (whose Default row keeps the standalone-hier). Each arm's worst cell now favours
  in-place-3 (0.959 against 0.940); a dedicated 8K board decides it next.

## HEAD against the change where the plans agree

`CRD_AB_HEAD=b15-head`, `CRD_AB_CANDIDATE=b15-cand3`, the planned forwards, same bits at every size:

| level | offset | 1K | 2K | 4K | 8K | 32K | 64K |
|---|---|---|---|---|---|---|---|
| avx512 | 0 | 1.014 [1.000-1.029] | 1.000 [1.000-1.000] | 1.000 [0.993-1.003] | 0.997 [0.997-1.007] | 1.006 [0.993-1.032] | 0.999 [0.990-1.024] |
| avx512 | 16 | 1.000 [0.986-1.014] | 1.028 [1.021-1.043] | 1.035 [1.029-1.038] | 0.998 [0.998-1.017] | 0.999 [0.998-1.020] | 1.004 [0.990-1.023] |
| avx2 | 0 | 1.000 [0.989-1.000] | 0.996 [0.986-1.011] | 1.000 [0.989-1.007] | 0.996 [0.994-0.998] | 0.991 [0.966-1.008] | 1.000 [0.987-1.021] |
| avx2 | 16 | 1.000 [0.989-1.011] | 0.987 [0.981-0.994] | 0.999 [0.997-1.005] | 0.998 [0.990-1.005] | 1.002 [0.997-1.031] | 1.003 [0.993-1.008] |

No cell regresses beyond the 2% band. The avx512 2K and 4K cells off a line read 1.03x faster with code that is
HEAD's there apart from the address test: the two binaries' layout, not a lever.
