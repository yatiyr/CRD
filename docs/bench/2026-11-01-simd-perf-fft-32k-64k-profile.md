# 2026-11-01 — SIMD-PERF: the f32 32K and 64K forwards against MKL, profiled (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-32k-64k-profile.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3. Two starts, 0.35 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO): HEAD
  (`~/ab/b18-head`, byte-identical to the last batch's `bcec86fd` build; `c591984a` changed only documents) and the
  candidate (`~/ab/b18-cand`, not kept): the in-place three passes offered and planned at 32K as n = 16·128·16, a
  generated 128-point pass 2 (`INPLACE_MIDS` with 128). `CRD_CPU_LEVEL=avx512` selects the fft unit.
- **Peer:** Intel MKL 2020.0.4 in the same process.
- **Method:** the profile loop (`bench_fft_f32 --loop`, 20,000 transforms at 32K and 10,000 at 64K, each from a fresh
  copy of the input) through `scripts/prof_fft_f32.sh`: top-down levels 1 and 2, `perf record`'s hottest symbols,
  and in passes of their own `mem_inst_retired.all_loads`/`all_stores`, `uops_dispatched.port_5_11`,
  `l1d.replacement`, `l2_rqsts.all_rfo`, `resource_stalls.sb` and `l1d_pend_miss.fb_full`. Counts are per transform,
  the copy included. The arms boards through `scripts/ab_fft_f32.sh` with `CRD_AB_ARMS` and `CRD_AB_REFERENCE=ip4`
  at `CRD_AB_OFFSET` 0 (a 64-byte boundary) and 16 (a malloc'd buffer): each timed transform warm, best of 200 a
  process, one warm-up, median paired ratio [percentile-bootstrap 95%], 2% band, at most 15 repetitions.

## Where the time goes (HEAD, avx512)

| n | side | cycles | instructions | GHz | memory bound | core bound | hottest |
|---|---|---|---|---|---|---|---|
| 32K | planned (ip4) | 175,041 | 351,630 | 2.90 | 28.2% | 29.8% | the IP4-AoS engine 86% |
| 32K | deep-split-3 | 176,789 | 276,035 | 2.93 | 63.1% | 9.0% | `codelet32_fused_notr_ss` 38%, `codelet32_batched_strided_sc` 24%, `codelet32_stage1_fused_sh_csf` 23% |
| 32K | deep-split-4 | 178,295 | 302,048 | 2.91 | 54.7% | 13.4% | `codelet16_fused_notr_ss` 46% |
| 32K | standalone-hier | 237,618 | 272,875 | 3.13 | 62.1% | 3.1% | `codelet128_batched_sc` 25%, the 256-point stage 1 51% |
| 32K | MKL | 146,793 | 206,356 | 2.93 | 49.4% | 25.5% | `rowbatch_twidl_fwd_032` 37%, `colbatch_twidl_fwd_32` 29%, `colbatch_plain_fwd_32` 19% |
| 32K | copy | 24,767 | 2,354 | 3.14 | 83.6% | 2.7% | `memmove` 90% |
| 64K | planned (ip4) | 410,036 | 744,014 | 2.92 | 19.2% | 43.9% | the IP4-AoS engine 87% |
| 64K | deep-split-3 | 408,790 | 585,585 | 2.93 | 60.0% | 11.4% | `codelet32_fused_notr_ss` 35%, the 32-point stage 1 25%, `codelet64_batched_strided_sc` 24% |
| 64K | deep-split-4 | 385,603 | 607,796 | 2.92 | 53.9% | 16.0% | `codelet16_fused_notr_ss` 46% |
| 64K | standalone-hier | 667,688 | 613,572 | 3.20 | 68.7% | 4.7% | the 256-point stage 1 and leaf |
| 64K | MKL | 331,331 | 446,922 | 2.91 | 49.6% | 24.2% | `rowbatch_twidl_fwd_032` 34%, `colbatch_twidl_fwd_32` 25%, `colbatch_plain_fwd_64` 23% |
| 64K | copy | 42,743 | 5,340 | 3.16 | 85.1% | 2.4% | `memmove` 91% |

MKL runs 32K as three passes of 32 and 64K as 32·32·64. The planned IP4-AoS runs 1.70x MKL's instructions at 32K and
1.66x at 64K, core bound. Cerid's own three-pass shape at MKL's sizes (the
deep-split-3, 32·32·32 at 32K) runs 1.34x MKL's instructions and ties IP4-AoS, memory bound.

## The in-place three passes at 32K (n = 16·128·16, not kept)

The 16K lever (the in-place three passes in the caller's buffer) needs equal first and last passes: pass 3 writes
the group of kb where it read it only when A = C (its inputs are elements C·kb + C·B·ka + i1, its outputs
A·kb + A·B·kc + ka). With the existing 16-point outer passes, 32K needs a 128-point pass 2.

| offset | ip4 ms | in-place-3 ms | MKL ms | in-place-3 against ip4 | MKL against ip4 |
|---|---|---|---|---|---|
| 0 | 0.05042 | 0.05254 | 0.04115 | 0.964 [0.946-0.972] slower | 1.217 [1.168-1.249] |
| 16 | 0.06227 | 0.06467 | 0.05393 | 0.967 [0.951-0.979] slower | 1.172 [1.149-1.211] |

Its profile (the candidate's loop, per transform):

| side | cycles | instructions | loads | stores | port 5 | `l1d.replacement` | memory bound | hottest |
|---|---|---|---|---|---|---|---|---|
| in-place-3 (16·128·16) | 199,480 | 272,029 | 79,146 | 49,680 | 88,767 | 41,747 | 58.0% | the 128-point pass 2 41.5%, pass 1 30.3%, pass 3 8.3% |
| ip4 | 174,952 | 350,723 | 89,714 | 49,586 | 126,263 | 50,178 | 30.6% | the IP4-AoS engine 84% |
| MKL | 155,300 | 206,096 | 38,676 | 17,269 | 80,976 | 41,429 | 52.9% | the three passes of 32 |

The 128-point leaf holds 256 vectors of 16 columns in 32 registers and spills: the transform stores 2.9x MKL's,
while its L1d fills equal MKL's. Reverted.

## What bounds the three-pass shapes

The same loop at 32K with the store-side events (one pass of its own a side, per transform):

| side | cycles | `l2_rqsts.all_rfo` | `resource_stalls.sb` | `l1d_pend_miss.fb_full` | stores |
|---|---|---|---|---|---|
| in-place-3 (candidate) | 198,147 | 17,159 | 106,104 | 13,995 | 49,675 |
| deep-split-3 | 175,420 | 18,750 | 89,600 | 13,236 | 41,937 |
| ip4 | 174,504 | 12,635 | 43,938 | 33,846 | 49,586 |
| MKL | 173,199 | 16,796 | 61,008 | 15,333 | 17,268 |
| copy | 21,111 | 4,103 | 16,678 | 7,216 | 247 |

A pass over the 256 KiB stores 4,096 lines, so three passes need about 12,300 stores; MKL makes 17,300. The
deep-split-3 makes 41,900 and the in-place candidate 49,700: their leaves spill. Their RFOs and fill-buffer stalls
match MKL's (the out-of-place intermediates cost no extra line traffic), and the store buffer is full for 51% and 54%
of their cycles against MKL's 35%. So placing the 32·32·32 shape in the caller's buffer, as at 16K, would remove no
traffic: the lever is the leaves' live set. A step of a Cerid leaf is 16 columns with real and imaginary parts in
their own registers, so a 32-point leaf keeps 64 vectors live in 32 registers (and a 16-point one 32 plus its
twiddles), while MKL's stores, 1.4x the three passes' own, say its leaves barely spill.

## The 64K arms (HEAD)

| offset | ip4 ms | MKL ms | deep-split-4 ms | deep-split-3 ms | MKL against ip4 | deep-split-4 against ip4 | deep-split-3 against ip4 | repetitions |
|---|---|---|---|---|---|---|---|---|
| 0 | 0.1203 | 0.08996 | 0.1165 | 0.1161 | 1.338 [1.318-1.347] | 1.029 [1.017-1.044] unresolved | 1.046 [1.001-1.064] unresolved | 15 |
| 16 | 0.1489 | 0.1198 | 0.1363 | 0.1386 | 1.239 [1.212-1.251] | 1.101 [1.048-1.112] faster | 1.071 [1.041-1.130] faster | 5 |

On a 64-byte boundary neither deep-split leaves the 2% band against IP4-AoS after the maximum repetitions, so the
64K plan stays; either would move 64K from 0.75x MKL to 0.77x to 0.78x, which the leaves' live set bounds as at 32K.
Planned against MKL: **32K 0.822x aligned and 0.853x off a line, 64K 0.747x and 0.807x**.
