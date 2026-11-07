# 2026-11-07 — HESAP-NET: RaptorQ against nanorq and the raptorq crate

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [RaptorQ](../sessions/2026-11-07-hesap-net-raptorq.md).

## Machine and arms

Intel Core i9-14900K, WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_RAPTORQ_VS_PEERS=ON`. Peers: nanorq at 7d52fbeb (its core with `-O3 -funroll-loops`, its kernels choosing AVX2 and
GFNI at run time) and raptorq 2.0.1 (Rust 1.99.1, release profile, its own detection of AVX2). hesap runs AVX2 with GFNI.
`crd-hesap-raptorq-board --rounds 7`, run twice: the range of the two, each the best of 7 interleaved rounds, on one block of
K symbols of 1280 octets. The board fails if an arm's symbols differ from hesap's. The crate's arm allocates a `Vec` per packet,
which is its API; the shim's copies around it are the only cost that is not the crate's.

## Throughput, Mbit/s of source data

Encode is a fresh plan, the intermediate symbols and 15% repair symbols. Planned leaves out the plan. Decode loses 15% of the
sources: a plan for the rows received, the intermediate symbols, the lost sources.

| Row | K | hesap | nanorq | crate | hesap over the best peer |
|---|---|---|---|---|---|
| Encode | 10 | 10,369 to 10,579 | 10,839 to 10,899 | 4,673 to 4,720 | 0.96x to 0.97x |
| Planned | 10 | 16,408 to 16,605 | 17,695 to 17,704 | 10,802 to 10,854 | 0.93x to 0.94x |
| Decode | 10 | 11,029 to 11,364 | 9,974 to 10,173 | 4,645 to 4,676 | 1.08x to 1.14x |
| Encode | 101 | 16,276 to 16,282 | 13,618 to 13,983 | 5,675 to 5,732 | 1.16x to 1.20x |
| Planned | 101 | 28,381 to 28,568 | 22,889 to 22,898 | 16,120 to 16,178 | 1.24x to 1.25x |
| Decode | 101 | 15,784 to 15,902 | 12,424 to 12,636 | 5,775 to 5,869 | 1.26x to 1.27x |
| Encode | 1000 | 14,437 to 15,004 | 11,856 to 11,923 | 6,026 to 6,264 | 1.21x to 1.27x |
| Planned | 1000 | 23,502 to 24,456 | 16,732 to 17,115 | 13,666 to 14,212 | 1.37x to 1.46x |
| Decode | 1000 | 15,449 to 15,898 | 10,852 to 10,898 | 6,175 to 6,242 | 1.42x to 1.47x |
| Encode | 10000 | 8,584 to 8,639 | 7,423 to 7,478 | 4,388 to 4,587 | 1.15x to 1.16x |
| Planned | 10000 | 11,396 to 11,799 | 9,902 to 10,073 | 8,504 to 8,555 | 1.13x to 1.19x |
| Decode | 10000 | 8,702 to 9,050 | 6,904 to 7,069 | 4,441 to 4,495 | 1.26x to 1.28x |
| Encode | 50000 | 3,953 to 4,002 | 3,382 to 3,387 | 2,291 to 2,321 | 1.17x to 1.18x |
| Planned | 50000 | 5,832 to 5,934 | 4,688 to 4,703 | 3,876 to 3,905 | 1.24x to 1.26x |
| Decode | 50000 | 3,486 to 3,567 | 3,016 to 3,106 | 2,163 to 2,218 | 1.12x to 1.18x |

The one row behind is K = 10, where nanorq leads by 3% to 7% on an encoder's plan. There u = 15 of L = 27 columns are dense, and
the block's cost is the u^2 products, in which neither code has an advantage. On a decoder's rows, with u = 12, hesap leads.
On AVX-512 that row is 0.79x and not yet explained: [AVX-512 board](2026-11-07-hesap-net-avx512.md).

Later the same day: these tables predate the [stall fix](2026-11-07-hesap-net-raptorq-stalls.md), which took every row ahead, this
one to 1.28x and AVX-512's 0.79x to 1.26x.

## Memory

hesap's plan, and everything beyond the symbols except a scratch of u + 2 symbols: nothing on the heap.

| K | L | P | u, encoder | u, decoder | Plan, encoder | Plan, decoder |
|---|---|---|---|---|---|---|
| 10 | 27 | 10 | 15 | 12 | 2.8 KB | 2.7 KB |
| 101 | 128 | 15 | 25 | 25 | 16.5 KB | 16.7 KB |
| 1000 | 1071 | 50 | 67 | 62 | 164 KB | 154 KB |
| 10000 | 10269 | 158 | 200 | 192 | 1.76 MB | 1.68 MB |
| 50000 | 51348 | 355 | 494 | 547 | 10.5 MB | 11.0 MB |
