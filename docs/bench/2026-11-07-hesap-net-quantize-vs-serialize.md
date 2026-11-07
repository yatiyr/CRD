# 2026-11-07 — HESAP-NET: quantisation against serialize and networked-physics

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [Quantisation](../sessions/2026-11-07-hesap-net-quantize.md).

## Machine and arms

Intel Core i9-14900K (AVX2, no AVX-512), WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_QUANTIZE_VS_PEERS=ON`. The peers are built without link-time optimisation: mas-bandwidth/serialize 2.1.3's
compressed_float, through its bit streams, and networked-physics' compressed_quaternion cut verbatim, whose decode stops
before its final `vectorial::normalize` (an SSE estimate of the CPU, which would only cost it more).
`crd-hesap-quantize-board --rounds 25`, run twice: 65,536 values of each arm, the best of 25 interleaved rounds. The board
fails if any arm's bits differ from the peer's. "+ its stream" is hesap's batch followed by serialize's own bit writer or
reader, the fair comparison with the serialize arm, which pays for its stream.

## Cost, nanoseconds per value

| Family | Arm | encode | decode | encode, peer / hesap | decode, peer / hesap |
|---|---|---|---|---|---|
| bounded float, 18 bits | serialize stream | 2.58 | 1.37 | 1.00 | 1.00 |
| | hesap sse2 + its stream | 0.73 | 0.71 | 3.51 | 1.92 |
| | hesap avx2 + its stream | 0.55 | 0.55 | 4.67 | 2.48 |
| | hesap sse2 alone | 0.36 | 0.28 | 7.22 | 4.84 |
| | hesap avx2 alone | 0.17 | 0.12 | 14.72 | 11.40 |
| | hesap portable alone | 1.51 | 0.59 | 1.70 | 2.34 |
| smallest three, 9 bits | networked-physics | 12.54 | 6.56 | 1.00 | 1.00 |
| | hesap sse2 | 3.44 | 5.98 | 3.64 | 1.10 |
| | hesap avx2 | 2.01 | 4.37 | 6.23 | 1.50 |
| | hesap portable | 22.01 | 11.23 | 0.57 | 0.58 |
| smallest three, 15 bits | networked-physics | 12.48 | 6.60 | 1.00 | 1.00 |
| | hesap sse2 | 3.34 | 5.97 | 3.73 | 1.10 |
| | hesap avx2 | 1.98 | 4.36 | 6.31 | 1.51 |
| | hesap portable | 21.76 | 11.00 | 0.57 | 0.60 |

The two runs agree within 0.1 ns on every arm above 1 ns.

## Reading

- Every level that a real CPU runs beats both peers. The floor is SSE2 on x64 and NEON on ARM64; the portable level is the
  reference of a scalar build, four simulated lanes with every select taken, and loses to the peers' branches by about 1.7x.
- The encode's gain is the peer's unpredictable branch on the largest component and its sign, which the selects remove.
- The decode's gain is small: each word's four fields and each quaternion's four components move through scalar code, since
  a word is 64 bits and the lanes 32. The peer also returns NaN where a word passes length one, at two bits and for every
  hostile word, and has no refusal; hesap's decode pays for both.
- No heap on any arm. The AVX-512 bytes are proven under SDE; its throughput waits for a real AVX-512 machine, as for the
  erasure kernels.
