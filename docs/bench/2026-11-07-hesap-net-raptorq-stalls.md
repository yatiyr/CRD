# 2026-11-07: RaptorQ's AVX-512 gap: store-forward stalls and line splits

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [RaptorQ stalls](../sessions/2026-11-07-hesap-net-raptorq-stalls.md).
Supersedes the throughput tables of the [first board](2026-11-07-hesap-net-raptorq-vs-peers.md) and the RaptorQ table of the
[AVX-512 board](2026-11-07-hesap-net-avx512.md); peers, arms and block as there.

## What held the row back

Spot c4-standard-8, Xeon Platinum 8581C, `crd-hesap-raptorq-board --k 10 --planned`, `perf record` on named events:

- `ld_blocks.store_forward`: 75% of the process's blocks were in hesap's `add`, about 330 per solve, nearly none in nanorq. Each row
  was `memcpy` or `memset` then additions. A constant `bytes` from GCC's IPA-CP made those `rep movsq` and `rep stosq` inside `solve`,
  and a 64-byte load of what a narrower store just wrote cannot forward. K = 10 was not resampled after the change; at K = 1000,
  2 samples remained in a 100-round run at a period of 100,003, none in hesap's kernels, and K = 10 gained 32% from the sum alone.
- Op counts per solve at K = 10: hesap 187 additions, 176 products, 19 copies, 20 zeroings; nanorq 125 additions, 26 entries of its
  first phase replayed, 257 products and 10 scalings, yet 22% faster: the op counts did not explain the gap.
- `ld_blocks.address_alias` (4K aliasing) was higher in nanorq, so not this. Its stride of 1344 is not the cause.
- With the stalls gone, line splits show: 64-byte aligned symbols against an 8-byte skew, native: K = 10 19,100 against 14,000,
  K = 1000 22,000 against 16,100, K = 10000 9,850 against 8,900 Mbit/s. nanorq aligns its own symbols. The first board's alignment
  experiment "moved nothing" only because the stalls dominated. hesap keeps any alignment, and raptorq.hpp names the cost.

The fix: `ErasureKernels::sum` sums up to 16 sources in one pass, each output vector stored once, replacing `add` in the table (`add`
is now a one-source `sum`). A row needs no copy or zeroing, and an intermediate symbol of d sources costs one store pass, not d.

## After, Mbit/s (`--rounds 7`, run twice; each the best of 7 interleaved rounds, hesap's symbols on 64-byte lines)

Xeon Platinum 8581C, one machine, 43 minutes, about 0.12 USD by list price, deleted with its disk in the same batch. hesap and nanorq
run AVX-512 GFNI.

| Row | K | hesap | nanorq | crate | hesap over the best |
|---|---|---|---|---|---|
| Encode | 10 | 9,103 to 9,536 | 6,552 to 6,708 | 2,802 to 2,824 | 1.39x to 1.42x |
| Planned | 10 | 17,382 to 18,544 | 13,799 to 13,815 | 8,559 to 8,701 | 1.26x to 1.34x |
| Decode | 10 | 9,655 to 9,919 | 6,230 to 6,249 | 2,920 to 3,053 | 1.55x to 1.59x |
| Encode | 101 | 11,935 to 11,982 | 7,694 to 7,738 | 3,140 to 3,187 | 1.55x |
| Planned | 101 | 29,157 to 29,771 | 15,900 to 15,936 | 9,274 to 9,302 | 1.83x to 1.87x |
| Decode | 101 | 11,551 to 11,618 | 6,751 to 6,890 | 3,299 to 3,321 | 1.69x to 1.71x |
| Encode | 1000 | 11,377 to 11,465 | 6,695 to 6,722 | 3,874 to 3,913 | 1.70x to 1.71x |
| Planned | 1000 | 21,632 to 21,891 | 11,300 to 11,338 | 9,781 to 9,853 | 1.91x to 1.93x |
| Decode | 1000 | 11,631 to 11,665 | 6,343 to 6,397 | 3,954 to 3,956 | 1.82x to 1.83x |
| Encode | 10000 | 6,437 to 6,501 | 4,277 to 4,292 | 2,994 to 3,017 | 1.51x |
| Planned | 10000 | 9,762 to 9,982 | 5,834 to 5,984 | 6,059 to 6,148 | 1.61x to 1.62x |
| Decode | 10000 | 6,669 to 6,772 | 3,944 to 3,979 | 3,098 to 3,127 | 1.69x to 1.70x |
| Encode | 50000 | 3,779 to 3,790 | 2,570 to 2,573 | 1,578 to 1,583 | 1.47x |
| Planned | 50000 | 7,828 to 7,843 | 4,308 to 4,319 | 3,039 to 3,044 | 1.81x to 1.82x |
| Decode | 50000 | 3,157 to 3,163 | 2,349 to 2,355 | 1,479 to 1,491 | 1.34x |

Before, unaligned: Planned K = 10 10,844, K = 101 15,328, K = 1000 12,022. Sum alone, unaligned: 14,600, 23,000, 16,000. Plain AVX2
tables (`CRD_CPU_LEVEL=avx2`; `avx512` likewise drops GFNI) now trail AVX-512 GFNI at K = 1000 too, 18,087 against 21,632.

Intel Core i9-14900K, WSL2, hesap and nanorq on AVX2 with GFNI, a busy desktop. The first run's K = 10 encode (0.99x) was noise at
start-up; the second gave 1.22x.

| Row | K | hesap | nanorq | crate | hesap over the best |
|---|---|---|---|---|---|
| Encode | 10 | 9,724 to 13,268 | 9,795 to 10,870 | 3,580 to 4,733 | 0.99x to 1.22x |
| Planned | 10 | 21,885 to 24,100 | 17,092 to 17,719 | 8,921 to 10,630 | 1.28x to 1.36x |
| Decode | 10 | 12,204 to 14,386 | 8,980 to 10,219 | 3,867 to 4,728 | 1.36x to 1.41x |
| Encode | 101 | 17,625 to 19,521 | 12,375 to 13,759 | 5,161 to 5,675 | 1.42x |
| Planned | 101 | 35,359 to 40,895 | 20,790 to 22,917 | 15,100 to 16,443 | 1.70x to 1.78x |
| Decode | 101 | 17,553 to 18,988 | 11,700 to 12,531 | 5,423 to 6,131 | 1.50x to 1.52x |
| Encode | 1000 | 16,453 to 18,703 | 11,342 to 11,879 | 5,670 to 6,269 | 1.45x to 1.57x |
| Planned | 1000 | 27,796 to 34,195 | 16,438 to 16,670 | 12,895 to 14,289 | 1.69x to 2.05x |
| Decode | 1000 | 16,440 to 19,668 | 9,787 to 11,051 | 5,723 to 6,239 | 1.68x to 1.78x |
| Encode | 10000 | 9,432 to 9,465 | 7,076 to 7,178 | 4,089 to 4,198 | 1.32x to 1.33x |
| Planned | 10000 | 12,894 to 13,514 | 8,952 to 9,664 | 6,867 to 7,699 | 1.40x to 1.44x |
| Decode | 10000 | 9,732 to 9,742 | 6,472 to 6,595 | 3,914 to 3,966 | 1.48x to 1.51x |
| Encode | 50000 | 4,723 to 4,828 | 3,021 to 3,040 | 2,150 to 2,161 | 1.56x to 1.59x |
| Planned | 50000 | 7,756 to 7,874 | 4,262 to 4,342 | 3,743 to 3,822 | 1.81x to 1.82x |
| Decode | 50000 | 4,220 to 4,282 | 2,835 to 2,856 | 2,047 to 2,091 | 1.48x to 1.51x |

## FlexFEC, which shares the kernel (`crd-hesap-flexfec-board --rounds 25`)

FlexFEC's payload XOR now sums each run of equal lengths in one pass. Xeon, two runs, against the [AVX-512 board](2026-11-07-hesap-net-avx512.md):

| Operation | hesap ns | libwebrtc / hesap | Before |
|---|---|---|---|
| six repairs | 852 to 864 | 2.41x to 2.42x | 1.92x |
| one repair over 24 | 477 to 504 | 2.84x | 2.38x |
| recover three | 2,241 to 2,299 | 3.47x to 3.55x | 3.35x |

The i9-14900K: 574 ns (2.09x), 381 ns (1.74x), 1,232 ns (3.50x).
