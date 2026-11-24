# 2026-11-24 — NET-PATH: a recovery period bursts the proven train, and version 7's turn-capped rows

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [recovery burst](../sessions/2026-11-24-net-path-recovery-burst.md).

## Method

The [train-cap board](2026-11-24-net-path-train-cap.md)'s method on the whole grid: `bottleneck_fixture.hpp`, one
flow of 1200-byte datagrams, HyStart++ on, bulk for 10 s, `linux-gcc-release` on WSL2 (i9-14900K); cells are link /
drops / reductions. "v7" is a version 7 policy; "before" is version 8 at 0a1b61b9 (a recovery period bursts the
initial window); "after" is this batch (a recovery period bursts the proven train). The v7 column reproduces the
train-cap board's cells exactly. Rows marked * differ between before and after; every other row is identical.

## The whole grid

| controller | link | host turn | queue | RTT | v7 | before (0a1b61b9) | after |
|---|---|---|---|---|---|---|---|
| CUBIC | 1 Gbit/s | exact | 12 KB | 2ms | 88.3% / 212 / 41 | 87.4% / 46 / 39 | 87.4% / 46 / 39 |
| CUBIC | 1 Gbit/s | exact | 12 KB | 20ms | 65.2% / 1827 / 3 | 65.2% / 1827 / 3 | 65.2% / 1827 / 3 |
| CUBIC | 1 Gbit/s | exact | 32 KiB | 2ms | 93.1% / 359 / 38 | 93.0% / 284 / 38 | 93.0% / 284 / 38 |
| CUBIC | 1 Gbit/s | exact | 32 KiB | 20ms | 68.6% / 2170 / 3 | 68.6% / 2170 / 3 | 68.6% / 2170 / 3 |
| CUBIC | 1 Gbit/s | exact | 64 KiB | 2ms | 98.0% / 402 / 33 | 98.0% / 348 / 32 | 98.0% / 348 / 32 |
| CUBIC | 1 Gbit/s | exact | 64 KiB | 20ms | 72.2% / 2516 / 3 | 72.2% / 2516 / 3 | 72.2% / 2516 / 3 |
| CUBIC | 1 Gbit/s | exact | 256 KiB | 2ms | 99.9% / 610 / 14 | 99.9% / 610 / 14 | 99.9% / 610 / 14 |
| CUBIC | 1 Gbit/s | exact | 256 KiB | 20ms | 85.3% / 3584 / 3 | 85.3% / 3587 / 3 | 85.3% / 3587 / 3 |
| CUBIC | 1 Gbit/s | 250 us | 12 KB | 2ms | 38.4% / 0 / 0 | 38.4% / 17 / 0 | 38.4% / 17 / 0 |
| CUBIC | 1 Gbit/s | 250 us | 12 KB | 20ms | 37.9% / 0 / 0 | 37.9% / 6 / 0 | 37.9% / 6 / 0 |
| CUBIC | 1 Gbit/s | 250 us | 32 KiB | 2ms | 38.4% / 0 / 0 | 99.7% / 123 / 1 | 99.7% / 123 / 1 |
| CUBIC | 1 Gbit/s | 250 us | 32 KiB | 20ms | 37.9% / 0 / 0 | 97.6% / 487 / 1 | 97.7% / 487 / 1 * |
| CUBIC | 1 Gbit/s | 250 us | 64 KiB | 2ms | 38.4% / 0 / 0 | 92.9% / 455 / 38 | 94.2% / 532 / 35 * |
| CUBIC | 1 Gbit/s | 250 us | 64 KiB | 20ms | 37.9% / 0 / 0 | 96.7% / 1553 / 0 | 96.7% / 1553 / 0 |
| CUBIC | 1 Gbit/s | 250 us | 256 KiB | 2ms | 38.4% / 0 / 0 | 99.7% / 532 / 16 | 99.9% / 581 / 15 * |
| CUBIC | 1 Gbit/s | 250 us | 256 KiB | 20ms | 37.9% / 0 / 0 | 93.0% / 2167 / 11 | 96.8% / 2771 / 6 * |
| CUBIC | 1 Gbit/s | 1 ms | 12 KB | 2ms | 9.6% / 0 / 0 | 9.6% / 12 / 0 | 9.6% / 12 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 12 KB | 20ms | 9.5% / 0 / 0 | 9.5% / 6 / 0 | 9.5% / 6 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 32 KiB | 2ms | 9.6% / 0 / 0 | 25.9% / 22 / 0 | 25.9% / 22 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 32 KiB | 20ms | 9.5% / 0 / 0 | 25.4% / 18 / 0 | 25.4% / 18 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 64 KiB | 2ms | 9.6% / 0 / 0 | 51.7% / 10 / 0 | 51.7% / 10 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 64 KiB | 20ms | 9.5% / 0 / 0 | 50.5% / 6 / 0 | 50.5% / 6 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 256 KiB | 2ms | 9.6% / 0 / 0 | 52.7% / 0 / 0 | 52.7% / 0 / 0 |
| CUBIC | 1 Gbit/s | 1 ms | 256 KiB | 20ms | 9.5% / 0 / 0 | 51.4% / 0 / 0 | 51.4% / 0 / 0 |
| CUBIC | 100 Mbit/s | exact | 12 KB | 2ms | 100.0% / 245 / 207 | 100.0% / 245 / 207 | 100.0% / 245 / 207 |
| CUBIC | 100 Mbit/s | exact | 12 KB | 20ms | 88.6% / 177 / 6 | 88.6% / 177 / 6 | 88.6% / 177 / 6 |
| CUBIC | 100 Mbit/s | exact | 32 KiB | 2ms | 100.0% / 154 / 87 | 100.0% / 154 / 87 | 100.0% / 154 / 87 |
| CUBIC | 100 Mbit/s | exact | 32 KiB | 20ms | 91.0% / 327 / 6 | 91.0% / 327 / 6 | 91.0% / 327 / 6 |
| CUBIC | 100 Mbit/s | exact | 64 KiB | 2ms | 100.0% / 142 / 38 | 100.0% / 142 / 38 | 100.0% / 142 / 38 |
| CUBIC | 100 Mbit/s | exact | 64 KiB | 20ms | 97.1% / 374 / 5 | 97.1% / 372 / 5 | 97.1% / 372 / 5 |
| CUBIC | 100 Mbit/s | exact | 256 KiB | 2ms | 100.0% / 146 / 5 | 100.0% / 146 / 5 | 100.0% / 146 / 5 |
| CUBIC | 100 Mbit/s | exact | 256 KiB | 20ms | 99.1% / 329 / 3 | 99.1% / 329 / 3 | 99.1% / 329 / 3 |
| CUBIC | 100 Mbit/s | 250 us | 12 KB | 2ms | 98.3% / 236 / 204 | 98.3% / 236 / 204 | 98.3% / 236 / 204 |
| CUBIC | 100 Mbit/s | 250 us | 12 KB | 20ms | 88.0% / 167 / 6 | 88.0% / 167 / 6 | 88.0% / 167 / 6 |
| CUBIC | 100 Mbit/s | 250 us | 32 KiB | 2ms | 100.0% / 154 / 87 | 100.0% / 154 / 87 | 100.0% / 154 / 87 |
| CUBIC | 100 Mbit/s | 250 us | 32 KiB | 20ms | 90.4% / 325 / 6 | 90.4% / 325 / 6 | 90.4% / 325 / 6 |
| CUBIC | 100 Mbit/s | 250 us | 64 KiB | 2ms | 100.0% / 141 / 38 | 100.0% / 141 / 38 | 100.0% / 141 / 38 |
| CUBIC | 100 Mbit/s | 250 us | 64 KiB | 20ms | 96.9% / 374 / 5 | 96.9% / 374 / 5 | 96.9% / 374 / 5 |
| CUBIC | 100 Mbit/s | 250 us | 256 KiB | 2ms | 100.0% / 138 / 5 | 100.0% / 138 / 5 | 100.0% / 138 / 5 |
| CUBIC | 100 Mbit/s | 250 us | 256 KiB | 20ms | 99.1% / 324 / 3 | 99.1% / 324 / 3 | 99.1% / 324 / 3 |
| CUBIC | 100 Mbit/s | 1 ms | 12 KB | 2ms | 95.9% / 0 / 0 | 95.9% / 12 / 0 | 95.9% / 12 / 0 |
| CUBIC | 100 Mbit/s | 1 ms | 12 KB | 20ms | 95.1% / 0 / 0 | 95.1% / 6 / 0 | 95.1% / 6 / 0 |
| CUBIC | 100 Mbit/s | 1 ms | 32 KiB | 2ms | 95.9% / 0 / 0 | 99.9% / 162 / 89 | 99.9% / 162 / 90 * |
| CUBIC | 100 Mbit/s | 1 ms | 32 KiB | 20ms | 95.1% / 0 / 0 | 92.3% / 142 / 11 | 92.3% / 142 / 11 |
| CUBIC | 100 Mbit/s | 1 ms | 64 KiB | 2ms | 95.9% / 0 / 0 | 99.9% / 214 / 42 | 99.9% / 218 / 42 * |
| CUBIC | 100 Mbit/s | 1 ms | 64 KiB | 20ms | 95.1% / 0 / 0 | 96.5% / 611 / 11 | 96.5% / 611 / 11 |
| CUBIC | 100 Mbit/s | 1 ms | 256 KiB | 2ms | 95.9% / 0 / 0 | 99.9% / 206 / 5 | 99.9% / 206 / 5 |
| CUBIC | 100 Mbit/s | 1 ms | 256 KiB | 20ms | 95.1% / 0 / 0 | 99.0% / 872 / 9 | 99.0% / 962 / 9 * |
| NewReno | 1 Gbit/s | exact | 12 KB | 2ms | 77.7% / 156 / 45 | 77.3% / 51 / 44 | 77.3% / 51 / 44 |
| NewReno | 1 Gbit/s | exact | 12 KB | 20ms | 52.3% / 1138 / 2 | 52.3% / 1138 / 2 | 52.3% / 1138 / 2 |
| NewReno | 1 Gbit/s | exact | 32 KiB | 2ms | 82.8% / 261 / 42 | 83.0% / 212 / 42 | 83.0% / 212 / 42 |
| NewReno | 1 Gbit/s | exact | 32 KiB | 20ms | 54.5% / 1382 / 2 | 54.5% / 1382 / 2 | 54.5% / 1382 / 2 |
| NewReno | 1 Gbit/s | exact | 64 KiB | 2ms | 89.1% / 302 / 37 | 89.2% / 269 / 37 | 89.2% / 269 / 37 |
| NewReno | 1 Gbit/s | exact | 64 KiB | 20ms | 56.8% / 1634 / 2 | 56.8% / 1634 / 2 | 56.8% / 1634 / 2 |
| NewReno | 1 Gbit/s | exact | 256 KiB | 2ms | 99.8% / 443 / 17 | 99.8% / 443 / 17 | 99.8% / 443 / 17 |
| NewReno | 1 Gbit/s | exact | 256 KiB | 20ms | 65.0% / 2439 / 2 | 65.1% / 2441 / 2 | 65.1% / 2441 / 2 |
| NewReno | 1 Gbit/s | 250 us | 12 KB | 2ms | 38.4% / 0 / 0 | 38.4% / 17 / 0 | 38.4% / 17 / 0 |
| NewReno | 1 Gbit/s | 250 us | 12 KB | 20ms | 37.9% / 0 / 0 | 37.9% / 6 / 0 | 37.9% / 6 / 0 |
| NewReno | 1 Gbit/s | 250 us | 32 KiB | 2ms | 38.4% / 0 / 0 | 99.7% / 123 / 1 | 99.7% / 123 / 1 |
| NewReno | 1 Gbit/s | 250 us | 32 KiB | 20ms | 37.9% / 0 / 0 | 97.6% / 487 / 1 | 97.7% / 487 / 1 * |
| NewReno | 1 Gbit/s | 250 us | 64 KiB | 2ms | 38.4% / 0 / 0 | 83.0% / 374 / 37 | 84.5% / 372 / 35 * |
| NewReno | 1 Gbit/s | 250 us | 64 KiB | 20ms | 37.9% / 0 / 0 | 96.7% / 1553 / 0 | 96.7% / 1553 / 0 |
| NewReno | 1 Gbit/s | 250 us | 256 KiB | 2ms | 38.4% / 0 / 0 | 99.3% / 456 / 17 | 99.5% / 474 / 17 * |
| NewReno | 1 Gbit/s | 250 us | 256 KiB | 20ms | 37.9% / 0 / 0 | 75.3% / 1742 / 6 | 93.1% / 2601 / 6 * |
| NewReno | 1 Gbit/s | 1 ms | 12 KB | 2ms | 9.6% / 0 / 0 | 9.6% / 12 / 0 | 9.6% / 12 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 12 KB | 20ms | 9.5% / 0 / 0 | 9.5% / 6 / 0 | 9.5% / 6 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 32 KiB | 2ms | 9.6% / 0 / 0 | 25.9% / 22 / 0 | 25.9% / 22 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 32 KiB | 20ms | 9.5% / 0 / 0 | 25.4% / 18 / 0 | 25.4% / 18 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 64 KiB | 2ms | 9.6% / 0 / 0 | 51.7% / 10 / 0 | 51.7% / 10 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 64 KiB | 20ms | 9.5% / 0 / 0 | 50.5% / 6 / 0 | 50.5% / 6 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 256 KiB | 2ms | 9.6% / 0 / 0 | 52.7% / 0 / 0 | 52.7% / 0 / 0 |
| NewReno | 1 Gbit/s | 1 ms | 256 KiB | 20ms | 9.5% / 0 / 0 | 51.4% / 0 / 0 | 51.4% / 0 / 0 |
| NewReno | 100 Mbit/s | exact | 12 KB | 2ms | 92.4% / 266 / 234 | 92.4% / 266 / 234 | 92.4% / 266 / 234 |
| NewReno | 100 Mbit/s | exact | 12 KB | 20ms | 75.0% / 117 / 6 | 75.0% / 117 / 6 | 75.0% / 117 / 6 |
| NewReno | 100 Mbit/s | exact | 32 KiB | 2ms | 100.0% / 155 / 107 | 100.0% / 155 / 107 | 100.0% / 155 / 107 |
| NewReno | 100 Mbit/s | exact | 32 KiB | 20ms | 81.6% / 224 / 5 | 81.6% / 224 / 5 | 81.6% / 224 / 5 |
| NewReno | 100 Mbit/s | exact | 64 KiB | 2ms | 100.0% / 122 / 47 | 100.0% / 122 / 47 | 100.0% / 122 / 47 |
| NewReno | 100 Mbit/s | exact | 64 KiB | 20ms | 86.9% / 270 / 5 | 86.9% / 269 / 5 | 86.9% / 269 / 5 |
| NewReno | 100 Mbit/s | exact | 256 KiB | 2ms | 100.0% / 131 / 6 | 100.0% / 131 / 6 | 100.0% / 131 / 6 |
| NewReno | 100 Mbit/s | exact | 256 KiB | 20ms | 99.0% / 269 / 3 | 99.0% / 269 / 3 | 99.0% / 269 / 3 |
| NewReno | 100 Mbit/s | 250 us | 12 KB | 2ms | 90.0% / 477 / 227 | 90.0% / 477 / 227 | 90.0% / 477 / 227 |
| NewReno | 100 Mbit/s | 250 us | 12 KB | 20ms | 74.5% / 111 / 6 | 74.5% / 111 / 6 | 74.5% / 111 / 6 |
| NewReno | 100 Mbit/s | 250 us | 32 KiB | 2ms | 99.9% / 156 / 107 | 99.9% / 156 / 107 | 99.9% / 156 / 107 |
| NewReno | 100 Mbit/s | 250 us | 32 KiB | 20ms | 80.9% / 223 / 5 | 80.9% / 223 / 5 | 80.9% / 223 / 5 |
| NewReno | 100 Mbit/s | 250 us | 64 KiB | 2ms | 100.0% / 134 / 47 | 100.0% / 134 / 47 | 100.0% / 134 / 47 |
| NewReno | 100 Mbit/s | 250 us | 64 KiB | 20ms | 86.4% / 272 / 5 | 86.4% / 272 / 5 | 86.4% / 272 / 5 |
| NewReno | 100 Mbit/s | 250 us | 256 KiB | 2ms | 100.0% / 126 / 6 | 100.0% / 126 / 6 | 100.0% / 126 / 6 |
| NewReno | 100 Mbit/s | 250 us | 256 KiB | 20ms | 98.9% / 267 / 3 | 98.9% / 267 / 3 | 98.9% / 267 / 3 |
| NewReno | 100 Mbit/s | 1 ms | 12 KB | 2ms | 95.9% / 0 / 0 | 95.9% / 12 / 0 | 95.9% / 12 / 0 |
| NewReno | 100 Mbit/s | 1 ms | 12 KB | 20ms | 95.1% / 0 / 0 | 95.1% / 6 / 0 | 95.1% / 6 / 0 |
| NewReno | 100 Mbit/s | 1 ms | 32 KiB | 2ms | 95.9% / 0 / 0 | 96.4% / 203 / 104 | 96.3% / 206 / 104 * |
| NewReno | 100 Mbit/s | 1 ms | 32 KiB | 20ms | 95.1% / 0 / 0 | 81.1% / 88 / 8 | 81.1% / 88 / 8 |
| NewReno | 100 Mbit/s | 1 ms | 64 KiB | 2ms | 95.9% / 0 / 0 | 99.4% / 222 / 48 | 99.4% / 213 / 48 * |
| NewReno | 100 Mbit/s | 1 ms | 64 KiB | 20ms | 95.1% / 0 / 0 | 89.9% / 449 / 7 | 89.9% / 449 / 7 |
| NewReno | 100 Mbit/s | 1 ms | 256 KiB | 2ms | 95.9% / 0 / 0 | 99.9% / 139 / 6 | 99.9% / 139 / 6 |
| NewReno | 100 Mbit/s | 1 ms | 256 KiB | 20ms | 95.1% / 0 / 0 | 98.8% / 650 / 6 | 98.8% / 653 / 6 * |

## Version 7's rows at 1 ms turns: a rate cap

Version 7 bursts 12000 bytes a turn: 96 Mbit/s at 1 ms turns. Sweeping the link across that cap (10 s each):

| controller | link | queue | RTT | v7, 1 ms turns | v7, exact | after, 1 ms turns | after, exact |
|---|---|---|---|---|---|---|---|
| CUBIC | 80 Mbit/s | 12 KB | 2ms | 89.5% / 205 / 191 | 100.0% / 228 / 215 | 89.5% / 207 / 191 | 100.0% / 228 / 215 |
| CUBIC | 80 Mbit/s | 12 KB | 20ms | 81.7% / 61 / 7 | 86.9% / 279 / 7 | 81.7% / 62 / 7 | 86.9% / 279 / 7 |
| CUBIC | 80 Mbit/s | 32 KiB | 2ms | 99.9% / 128 / 87 | 100.0% / 146 / 86 | 99.9% / 132 / 86 | 100.0% / 146 / 86 |
| CUBIC | 80 Mbit/s | 32 KiB | 20ms | 91.7% / 103 / 7 | 93.6% / 328 / 7 | 91.7% / 118 / 7 | 93.6% / 328 / 7 |
| CUBIC | 80 Mbit/s | 64 KiB | 2ms | 99.9% / 100 / 36 | 100.0% / 133 / 34 | 99.9% / 137 / 35 | 100.0% / 133 / 34 |
| CUBIC | 80 Mbit/s | 64 KiB | 20ms | 97.7% / 141 / 6 | 98.5% / 352 / 6 | 97.9% / 176 / 6 | 98.5% / 352 / 6 |
| CUBIC | 90 Mbit/s | 12 KB | 2ms | 85.7% / 186 / 181 | 100.0% / 221 / 211 | 85.7% / 188 / 181 | 100.0% / 221 / 211 |
| CUBIC | 90 Mbit/s | 12 KB | 20ms | 65.9% / 36 / 8 | 85.4% / 226 / 7 | 65.9% / 37 / 8 | 85.4% / 226 / 7 |
| CUBIC | 90 Mbit/s | 32 KiB | 2ms | 99.9% / 105 / 88 | 100.0% / 149 / 85 | 99.9% / 108 / 87 | 100.0% / 149 / 85 |
| CUBIC | 90 Mbit/s | 32 KiB | 20ms | 90.6% / 48 / 7 | 92.5% / 317 / 6 | 90.6% / 81 / 7 | 92.5% / 317 / 6 |
| CUBIC | 90 Mbit/s | 64 KiB | 2ms | 99.9% / 75 / 41 | 100.0% / 137 / 36 | 99.9% / 128 / 38 | 100.0% / 137 / 36 |
| CUBIC | 90 Mbit/s | 64 KiB | 20ms | 96.8% / 80 / 8 | 97.7% / 351 / 6 | 97.1% / 96 / 7 | 97.7% / 351 / 6 |
| CUBIC | 96 Mbit/s | 12 KB | 2ms | 99.9% / 0 / 0 | 100.0% / 237 / 199 | 99.9% / 12 / 0 | 100.0% / 237 / 199 |
| CUBIC | 96 Mbit/s | 12 KB | 20ms | 99.0% / 0 / 0 | 84.5% / 193 / 6 | 99.0% / 6 / 0 | 84.5% / 193 / 6 |
| CUBIC | 96 Mbit/s | 32 KiB | 2ms | 99.9% / 0 / 0 | 100.0% / 152 / 87 | 99.9% / 22 / 0 | 100.0% / 152 / 87 |
| CUBIC | 96 Mbit/s | 32 KiB | 20ms | 99.0% / 0 / 0 | 91.6% / 320 / 6 | 99.0% / 28 / 0 | 91.6% / 320 / 6 |
| CUBIC | 96 Mbit/s | 64 KiB | 2ms | 99.9% / 0 / 0 | 100.0% / 141 / 38 | 99.9% / 84 / 0 | 100.0% / 141 / 38 |
| CUBIC | 96 Mbit/s | 64 KiB | 20ms | 99.0% / 0 / 0 | 97.4% / 361 / 6 | 99.0% / 166 / 0 | 97.4% / 361 / 6 |
| CUBIC | 100 Mbit/s | 12 KB | 2ms | 95.9% / 0 / 0 | 100.0% / 245 / 207 | 95.9% / 12 / 0 | 100.0% / 245 / 207 |
| CUBIC | 100 Mbit/s | 12 KB | 20ms | 95.1% / 0 / 0 | 88.6% / 177 / 6 | 95.1% / 6 / 0 | 88.6% / 177 / 6 |
| CUBIC | 100 Mbit/s | 32 KiB | 2ms | 95.9% / 0 / 0 | 100.0% / 154 / 87 | 99.9% / 162 / 90 | 100.0% / 154 / 87 |
| CUBIC | 100 Mbit/s | 32 KiB | 20ms | 95.1% / 0 / 0 | 91.0% / 327 / 6 | 92.3% / 142 / 11 | 91.0% / 327 / 6 |
| CUBIC | 100 Mbit/s | 64 KiB | 2ms | 95.9% / 0 / 0 | 100.0% / 142 / 38 | 99.9% / 218 / 42 | 100.0% / 142 / 38 |
| CUBIC | 100 Mbit/s | 64 KiB | 20ms | 95.1% / 0 / 0 | 97.1% / 374 / 5 | 96.5% / 611 / 11 | 97.1% / 372 / 5 |
| CUBIC | 110 Mbit/s | 12 KB | 2ms | 87.2% / 0 / 0 | 99.5% / 235 / 201 | 87.2% / 12 / 0 | 99.5% / 235 / 201 |
| CUBIC | 110 Mbit/s | 12 KB | 20ms | 86.4% / 0 / 0 | 83.0% / 201 / 6 | 86.4% / 6 / 0 | 86.7% / 136 / 5 |
| CUBIC | 110 Mbit/s | 32 KiB | 2ms | 87.2% / 0 / 0 | 100.0% / 157 / 88 | 99.8% / 190 / 90 | 100.0% / 157 / 88 |
| CUBIC | 110 Mbit/s | 32 KiB | 20ms | 86.4% / 0 / 0 | 90.5% / 358 / 6 | 91.7% / 319 / 12 | 90.5% / 358 / 6 |
| CUBIC | 110 Mbit/s | 64 KiB | 2ms | 87.2% / 0 / 0 | 100.0% / 147 / 40 | 99.9% / 203 / 44 | 100.0% / 147 / 40 |
| CUBIC | 110 Mbit/s | 64 KiB | 20ms | 86.4% / 0 / 0 | 96.4% / 410 / 5 | 96.4% / 609 / 11 | 96.4% / 410 / 5 |
| CUBIC | 150 Mbit/s | 12 KB | 2ms | 64.0% / 0 / 0 | 98.4% / 190 / 170 | 64.0% / 12 / 0 | 98.4% / 189 / 170 |
| CUBIC | 150 Mbit/s | 12 KB | 20ms | 63.4% / 0 / 0 | 83.3% / 395 / 5 | 63.4% / 6 / 0 | 83.3% / 395 / 5 |
| CUBIC | 150 Mbit/s | 32 KiB | 2ms | 64.0% / 0 / 0 | 100.0% / 172 / 91 | 97.5% / 169 / 92 | 100.0% / 172 / 91 |
| CUBIC | 150 Mbit/s | 32 KiB | 20ms | 63.4% / 0 / 0 | 88.7% / 526 / 5 | 88.2% / 196 / 12 | 88.7% / 526 / 5 |
| CUBIC | 150 Mbit/s | 64 KiB | 2ms | 64.0% / 0 / 0 | 100.0% / 163 / 45 | 99.9% / 256 / 46 | 100.0% / 163 / 45 |
| CUBIC | 150 Mbit/s | 64 KiB | 20ms | 63.4% / 0 / 0 | 94.5% / 597 / 5 | 93.0% / 568 / 12 | 94.5% / 597 / 5 |
| NewReno | 80 Mbit/s | 12 KB | 2ms | 80.5% / 414 / 204 | 94.6% / 260 / 248 | 80.5% / 416 / 204 | 94.6% / 260 / 248 |
| NewReno | 80 Mbit/s | 12 KB | 20ms | 73.6% / 42 / 6 | 76.7% / 189 / 7 | 73.6% / 43 / 6 | 76.7% / 189 / 7 |
| NewReno | 80 Mbit/s | 32 KiB | 2ms | 98.6% / 222 / 103 | 100.0% / 148 / 105 | 98.6% / 237 / 103 | 100.0% / 148 / 105 |
| NewReno | 80 Mbit/s | 32 KiB | 20ms | 80.9% / 65 / 6 | 83.5% / 227 / 6 | 80.8% / 60 / 6 | 83.5% / 227 / 6 |
| NewReno | 80 Mbit/s | 64 KiB | 2ms | 99.9% / 80 / 43 | 100.0% / 113 / 42 | 99.9% / 128 / 42 | 100.0% / 113 / 42 |
| NewReno | 80 Mbit/s | 64 KiB | 20ms | 89.5% / 95 / 6 | 91.2% / 255 / 6 | 89.5% / 110 / 6 | 91.2% / 255 / 6 |
| NewReno | 90 Mbit/s | 12 KB | 2ms | 77.7% / 325 / 193 | 93.5% / 251 / 241 | 77.7% / 327 / 193 | 93.5% / 251 / 241 |
| NewReno | 90 Mbit/s | 12 KB | 20ms | 59.1% / 24 / 7 | 75.6% / 151 / 6 | 59.1% / 25 / 7 | 75.6% / 151 / 6 |
| NewReno | 90 Mbit/s | 32 KiB | 2ms | 98.1% / 113 / 105 | 100.0% / 150 / 104 | 98.1% / 121 / 104 | 100.0% / 150 / 104 |
| NewReno | 90 Mbit/s | 32 KiB | 20ms | 80.4% / 33 / 6 | 81.5% / 219 / 6 | 80.3% / 66 / 6 | 81.5% / 219 / 6 |
| NewReno | 90 Mbit/s | 64 KiB | 2ms | 99.9% / 65 / 46 | 100.0% / 117 / 44 | 99.9% / 125 / 45 | 100.0% / 117 / 44 |
| NewReno | 90 Mbit/s | 64 KiB | 20ms | 87.0% / 53 / 6 | 89.0% / 255 / 5 | 87.4% / 64 / 6 | 89.0% / 255 / 5 |
| NewReno | 96 Mbit/s | 12 KB | 2ms | 99.9% / 0 / 0 | 93.8% / 260 / 228 | 99.9% / 12 / 0 | 93.8% / 260 / 228 |
| NewReno | 96 Mbit/s | 12 KB | 20ms | 99.0% / 0 / 0 | 75.0% / 129 / 6 | 99.0% / 6 / 0 | 75.0% / 129 / 6 |
| NewReno | 96 Mbit/s | 32 KiB | 2ms | 99.9% / 0 / 0 | 100.0% / 154 / 107 | 99.9% / 22 / 0 | 100.0% / 154 / 107 |
| NewReno | 96 Mbit/s | 32 KiB | 20ms | 99.0% / 0 / 0 | 82.0% / 220 / 6 | 99.0% / 28 / 0 | 82.0% / 220 / 6 |
| NewReno | 96 Mbit/s | 64 KiB | 2ms | 99.9% / 0 / 0 | 100.0% / 120 / 46 | 99.9% / 84 / 0 | 100.0% / 120 / 46 |
| NewReno | 96 Mbit/s | 64 KiB | 20ms | 99.0% / 0 / 0 | 87.6% / 262 / 5 | 99.0% / 166 / 0 | 87.6% / 262 / 5 |
| NewReno | 100 Mbit/s | 12 KB | 2ms | 95.9% / 0 / 0 | 92.4% / 266 / 234 | 95.9% / 12 / 0 | 92.4% / 266 / 234 |
| NewReno | 100 Mbit/s | 12 KB | 20ms | 95.1% / 0 / 0 | 75.0% / 117 / 6 | 95.1% / 6 / 0 | 75.0% / 117 / 6 |
| NewReno | 100 Mbit/s | 32 KiB | 2ms | 95.9% / 0 / 0 | 100.0% / 155 / 107 | 96.3% / 206 / 104 | 100.0% / 155 / 107 |
| NewReno | 100 Mbit/s | 32 KiB | 20ms | 95.1% / 0 / 0 | 81.6% / 224 / 5 | 81.1% / 88 / 8 | 81.6% / 224 / 5 |
| NewReno | 100 Mbit/s | 64 KiB | 2ms | 95.9% / 0 / 0 | 100.0% / 122 / 47 | 99.4% / 213 / 48 | 100.0% / 122 / 47 |
| NewReno | 100 Mbit/s | 64 KiB | 20ms | 95.1% / 0 / 0 | 86.9% / 270 / 5 | 89.9% / 449 / 7 | 86.9% / 269 / 5 |
| NewReno | 110 Mbit/s | 12 KB | 2ms | 87.2% / 0 / 0 | 91.4% / 256 / 227 | 87.2% / 12 / 0 | 91.4% / 256 / 227 |
| NewReno | 110 Mbit/s | 12 KB | 20ms | 86.4% / 0 / 0 | 74.5% / 128 / 5 | 86.4% / 6 / 0 | 77.2% / 92 / 5 |
| NewReno | 110 Mbit/s | 32 KiB | 2ms | 87.2% / 0 / 0 | 100.0% / 159 / 109 | 94.2% / 230 / 103 | 100.0% / 159 / 109 |
| NewReno | 110 Mbit/s | 32 KiB | 20ms | 86.4% / 0 / 0 | 79.6% / 245 / 5 | 81.5% / 265 / 8 | 79.6% / 245 / 5 |
| NewReno | 110 Mbit/s | 64 KiB | 2ms | 87.2% / 0 / 0 | 100.0% / 126 / 49 | 99.7% / 166 / 50 | 100.0% / 126 / 49 |
| NewReno | 110 Mbit/s | 64 KiB | 20ms | 86.4% / 0 / 0 | 86.2% / 294 / 5 | 88.8% / 418 / 7 | 86.2% / 294 / 5 |
| NewReno | 150 Mbit/s | 12 KB | 2ms | 64.0% / 0 / 0 | 90.1% / 210 / 193 | 64.0% / 12 / 0 | 90.1% / 210 / 193 |
| NewReno | 150 Mbit/s | 12 KB | 20ms | 63.4% / 0 / 0 | 74.3% / 258 / 4 | 63.4% / 6 / 0 | 74.3% / 258 / 4 |
| NewReno | 150 Mbit/s | 32 KiB | 2ms | 64.0% / 0 / 0 | 99.2% / 170 / 112 | 89.0% / 217 / 99 | 99.2% / 170 / 112 |
| NewReno | 150 Mbit/s | 32 KiB | 20ms | 63.4% / 0 / 0 | 78.1% / 358 / 4 | 79.7% / 149 / 8 | 78.1% / 358 / 4 |
| NewReno | 150 Mbit/s | 64 KiB | 2ms | 64.0% / 0 / 0 | 100.0% / 140 / 55 | 99.1% / 219 / 55 | 100.0% / 140 / 55 |
| NewReno | 150 Mbit/s | 64 KiB | 20ms | 63.4% / 0 / 0 | 82.7% / 415 / 4 | 83.6% / 340 / 7 | 82.7% / 415 / 4 |

## Reading

- Recovery (operator item c): on the deep 20 ms rows the period held a coarse host to the initial window, 12000
  bytes a 250 us turn: NewReno at 1 Gbit/s and 256 KiB spent 0.14 s of 10 s in 6 recovery periods sending at 357
  Mbit/s, and with the proven train at 880 Mbit/s (6.3 against 15.5 MB in those periods); the ACK clock it kept
  made the rest of the difference, 75.3% to 93.1% (CUBIC 93.0% to 96.8%, 11 reductions to 6). Bursting the whole
  cap in recovery (no rule) gets the same there but cuts CUBIC's 100 Mbit/s, 12 KB, 20 ms row with exact timers to
  83.4% (88.6% both before and after): a growth still unproven overflowed the queue the reduction left full.
- The one row after is below before by 0.1 (NewReno, 100 Mbit/s, 1 ms turns, 32 KiB, 2 ms: 96.4% to 96.3%). Over
  27 nearby paths (98 to 102 Mbit/s, the queue plus 0, 600 or 1200 bytes) the two rules' means differ by at most
  0.05 points either way and each wins on some: path detail, not the rule. CUBIC 100 Mbit/s 256 KiB 20 ms drops 962
  against 872 at the same 99.0%.
- Version 7's 95.1% and 95.9% at 100 Mbit/s and 1 ms turns, with no drop, are its 96 Mbit/s turn cap sitting under
  the link: at 96 Mbit/s it gets 99.9% and 99.0%; under the cap (80, 90 Mbit/s) it fills the queue, drops and
  reduces like any controller and version 8 gets the same within 0.1; above it (110, 150 Mbit/s) the cap leaves 13
  to 37% of the link unused, which version 8 fills. On the 100 Mbit/s link version 8 at 1 ms turns gets what both
  versions get with exact timers (CUBIC 32 KiB 20 ms 92.3% against 91.0%, NewReno 81.1% against 81.6%, NewReno
  64 KiB 89.9% against 86.9%).
- Drops where version 7 dropped none (1 Gbit/s, 250 us turns, deep queues): version 7 is held to 38% there and
  never fills a queue. With exact timers, where it does, it drops too: 32 KiB at 2 ms 359 at 93.1% of the link,
  where version 8 at 250 us turns drops 123 at 99.7%; 256 KiB at 20 ms 3,584 at 85.3%, version 8 2,771 at 96.8%.
- The exact-timer row (CUBIC, 1 Gbit/s, 12 KB, 2 ms: 87.4% against 88.3%; NewReno 77.3% against 77.7%) is not the
  recovery rule: it is the first 250 ms. Version 8's slow start bursts 24000-byte trains (the cap's first doubling)
  into the 12 KB queue, loses 8 packets and leaves slow start at a 114,000-byte window (1 reduction); version 7 runs
  on to 406,800 bytes and loses 174 (3 reductions). Quarter-second rates after that: 88.4% against 88.2 to 88.7%. Open.
- Restarts (the restart board's sixteen rows and sixteen more, 8 MiB then twenty MiB writes a second apart): every
  row identical before and after.
- QUIC board (`quic_board.py --rounds 5`, interleaved with f5cf140a's build, whose engine differs from 0a1b61b9's
  only in path validation): 64 KiB windows 415 against 411 MiB per CPU-second and 651 against 669 per wall second;
  1 MiB windows 562 against 574 and 983 against 908. Within the rounds' spread: loopback enters no recovery period.
