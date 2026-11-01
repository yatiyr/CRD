# 2026-11-01 — SIMD-PERF: the f32 1K forward's stage-1 transposed store a register a row at avx512 (real AVX-512)

<!-- doc-role: evidence -->
> Dated evidence; counts and results are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).
> Owner row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf); session:
> [2026-11-01](../sessions/2026-11-01-simd-perf-fft-stage1-transpose.md); method:
> [fast A/B boards](README.md#fast-ab-boards). Never compare these numbers with the i9-14900K boards; ratios only.

- **Machine:** the optimization track's benchmark machine, as in the
  [first board](2026-10-31-simd-perf-fft-f32-xeon.md): GCE spot `c4-standard-8`, Intel Xeon Platinum 8581C (Emerald
  Rapids), L1d 48 KiB and L2 2 MiB a core, Ubuntu 24.04, GCC 13.3, perf with the guest PMU. **Real AVX-512
  hardware.** Every process pinned to CPU 3; the host idle before each board (28.6 of 29.4 GiB available). One
  start, 0.63 h.
- **Binaries:** `bench_fft_f32` from the linux-gcc-release lane tree (GCC 13.3, `-O3`, program LTO): HEAD `f501fb16`
  (`~/ab/b17-head`; the tree's ninja found it current at that commit), the split candidate (`~/ab/b17-cand`: the
  standalone-hier's 1K as 16 x 64, not kept) and the change (`~/ab/b17-cand2`: stage 1's transposed store as one
  `Vec16f` a row). `CRD_CPU_LEVEL=avx512` selects the fft unit. The change is in the avx512 build of the codelet
  lanes only, so avx2, sse2 and portable run HEAD's code and were not boarded.
- **Peer:** Intel MKL 2020.0.4 in the same process.
- **Method:** `ab_fft_f32.sh` at `CRD_AB_OFFSET` 0 (a cache-line-aligned buffer) and 16 (a malloc'd one): HEAD's
  and the change's planned forwards with `--same` on the output digests (each timed transform warm, best of 200 a
  process, paired ratios over 15 processes, percentile-bootstrap 95%, 2% band); the candidate's named arms against
  HEAD's (`CRD_AB_CANDIDATE` with `CRD_AB_ARMS`, added in this batch for a change that alters an algorithm's bits);
  `prof_fft_f32.sh` (top-down levels 1 and 2, port 5 and port 1 dispatches, retired loads and stores, `perf
  record`'s hottest symbols; per transform, the input copy included); `objdump` of the 1K stage-1 codelet.

## The split first (16 x 64, not kept)

The [1K profile](2026-11-01-simd-perf-fft-1k-8k-profile.md) named MKL's 64 x 16 shape. The standalone-hier at
1024 = 16 x 64 needs no new codelet (the 16-point stage 1 and the 64-point final leaf exist), so it was tried as the
split: same algorithm, other bits (1.2e-07 against the FFTW f64 truth).

| side | cycles | instructions | port 5 | hottest |
|---|---|---|---|---|
| HEAD, 32 x 32 | 2,260 | 6,213 | 2,191 | stage 1 `codelet32_stage1_fused_sh_cs` 61.6%, `codelet32_batched_sc` 22.2% |
| 16 x 64 | 2,300 | 6,114 | 2,223 | stage 1 `codelet16_stage1_fused_sh_cs` 60.6%, `codelet64_batched_sc` 28.8% |

On the board the split ran 0.920x HEAD [0.908-0.921] (three repetitions, decided). Port 5 did not drop: stage 1's
share stayed at 60% whatever its leaf, so the bound is its transposed store, which moves every element once at any
split. Reverted.

## Where stage 1's port-5 operations went

At avx512 a step of stage 1 is two tiles (16 columns, one `Vec16f` a row), and each group of eight outputs is
stored transposed into the AoSoA rows. HEAD split the eight rows into 256-bit halves (8 `vextractf32x8`) and ran
Vec8f's 8x8 transpose on each half (48 shuffles, the 128-bit lane stage as `vperm2f128`): 56 port-5 operations a
group, 448 a step of the 32-point stage 1 (8 groups: four of real parts, four of imaginary parts), two steps a 1K
transform. `transpose8x8_halves` (landed for the in-place pass 3, `b0640d0c`) runs both 8x8 transposes on whole
registers in 24 operations, after which a row's low half is one column's store and its high half the other's; the
compiler folds every high-half extract into its store (`vextractf32x8 $1, zmm, mem`: 64 in the codelet, all to
memory, no port-5 operation). The codelet's shuffles went from 448 to 256 (the 64 `vpermt2ps` of the deinterleave
remain).

## The change against HEAD (the same bits)

The loop at avx512, per transform with the copy:

| n | side | cycles | instructions | port 5 | port 1 | stores | microseconds |
|---|---|---|---|---|---|---|---|
| 1K | HEAD | 2,257 | 6,212 | 2,190 | 111 | 904 | 0.733 |
| 1K | change | 1,766 | 5,659 | 1,718 | 117 | 903 | 0.571 |
| 256K | HEAD | 2,833,000 | 2,869,000 | 1,013,000 | 84,000 | 395,700 | 848 |
| 256K | change | 2,863,000 | 2,733,000 | 910,000 | 86,000 | 395,800 | 858 |

The board, the change's planned forward against HEAD's (above 1 the change is faster). HEAD and the change printed
the same digests at every size of every repetition at both offsets:

| n | planned forward | aligned | verdict | 16 bytes off a line | verdict |
|---|---|---|---|---|---|
| 1K | standalone-hier | 1.222 [1.19-1.232] | faster | 1.200 [1.193-1.237] | faster |
| 2K | in-place-3 | 1.000 [0.977-1.031] | unresolved | 0.993 [0.987-1.000] | level |
| 4K | in-place-3 | 1.007 [0.976-1.038] | unresolved | 1.006 [0.979-1.015] | unresolved |
| 8K | in-place-3 | 0.998 [0.977-1.026] | unresolved | 1.005 [0.954-1.079] | unresolved |
| 16K | in-place-3 | 1.003 [0.990-1.021] | unresolved | 1.003 [0.995-1.005] | level |
| 32K | ip4 | 0.996 [0.995-1.008] | level | 1.003 [0.997-1.006] | level |
| 64K | ip4 | 1.001 [0.990-1.010] | level | 0.998 [0.990-1.002] | level |
| 128K | deep-split-4 | 0.977 [0.971-1.010] | unresolved | 0.987 [0.982-0.991] | level |
| 256K | deep-split-4 | 0.990 [0.988-0.996] | level | 0.994 [0.987-0.995] | level |
| 512K | deep-split-4 | 0.992 [0.990-0.995] | level | 0.994 [0.992-0.996] | level |
| 1M | tiled-four-step | 0.995 [0.994-0.997] | level | 0.993 [0.991-0.999] | level |

The plans that store no stage-1 group (the in-place passes at 2K to 16K, IP4-AoS at 32K and 64K) are the control:
level, their intervals around 1. The deep-split-4 and the tiled four-step store theirs the new way, and their
intervals sit 0.5% to 1.3% below 1, inside the 2% band: memory bound there, the change removes 4.7% of the 256K
forward's instructions and 10% of its port-5 operations, and the transform takes 1.1% more cycles. A small real
cost at those sizes, recorded and left for its own board; there Cerid leads MKL 1.3x to 1.9x.

## Against MKL

| board | n | aligned | 16 bytes off a line |
|---|---|---|---|
| HEAD's planned forward | 1K | 0.788x (MKL 1.269 [1.25-1.278]) | 0.789x (MKL 1.268 [1.259-1.286]) |
| the change's planned forward (`--planned`) | 1K | 0.947 [0.946-0.966] | 0.965 [0.948-0.966] |
| the change's standalone-hier among every offered arm | 1K | 0.930 [0.918-0.934] | 0.938 [0.923-0.967] |

The planned board times the plan and MKL alone in a process; the named-arms board times every forward the planner
offers at 1K in the same process, so its cache holds the other arms' tables too. Either way 1K moves from 0.79x MKL
to 0.93x to 0.97x.
