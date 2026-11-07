# 2026-11-07: RaptorQ per RFC 6330, against nanorq and the raptorq crate

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 108.

## CI first

Nothing new from this track: every failure in dispatch 42328203363 (9a058d80) is older than its fix (crd-ci-platform 67e30587;
test_clock b565a3b6; the rest is the registered mikktspace oracle). The run is evidence for two of the row's open items:
`crd-hesap-net-kernels-at-neon` passes on linux-gcc-arm64-debug and win-arm64-debug (erasure and quantize; motion landed later),
and linux-gcc-public-checks passes the libwebrtc, ntpd and networked-physics peers, with libwebrtc's install step.

## What landed, in capability `numerics.erasure-codes`, no allocation

- `raptorq.hpp`, `src/raptorq.cpp`. The tables are generated from the RFC's text (`scripts/gen-raptorq-tables.py`, SHA-256 pinned).
- `RaptorqSolver`: §5.4.2's inactivation decoding in block form. The first phase never creates a nonzero in V, so V1, V2, U1 and U2
  stay A's own entries (the raptorq crate's errata 9 to 11): only the Schur complement is dense, a plan solves any number of blocks
  in 2·nonzeros XORs and u^2 products, and memory is three spans sized by calls, at any alignment.
- §3's payload ID and OTI, §4.3's derivation, §4.4.1.2's partition and sub-symbol interleaving.
- Verified erratum 5548: F is at most 942574504275, 255 blocks, not the RFC's 946270874880. The RFC's errata page was read.

## Evidence

- `crd-hesap-net-tests` `[raptorq]`, 24 cases: A·C = D on the literal matrix of Figure 6; Enc reproduces the sources of every K' to
  2000; the verdict equals the literal rank on 8,000 sets (§5.8's "mathematically sufficient"); a decoder's C is the encoder's;
  §5.8's rates at K', K' + 1 and K' + 2; K = 56403 encoded and decoded with half the sources lost. Clean, one digest, on win-debug,
  linux-gcc-debug, linux-gcc-release, linux-gcc-asan, linux-clang-tsan, linux-clang-fuzz; the forced arms hold the same digest.
- `crd-hesap-raptorq-peer-tests` (linux-gcc-public-checks): nanorq's parameters and tuples for every K', and its symbols byte for
  byte on all 477 K' of Table 2. Both peers' repair symbols byte for byte on padded and large blocks, the last ESIs and symbol sizes
  1 to 65535. Each decoder recovers from another's symbols, and all three give one verdict on 3,000 sets of K rows.
- `crd-fuzz-hesap-raptorq`, 32 seeds: 808,136 libFuzzer executions clean under ASan and UBSan. Its first replay found a defect: a
  block past the object's last symbol reported an offset past the end. It now starts at the end.
- 44 teeth, all caught, with the peers out of the loop. Five first survived (size slack, Z and N at their exact bounds, the 255
  block limit, the ESI's top bit), so the tests gained sentinels past each span and those exact cases.
- [Board](../bench/2026-11-07-hesap-net-raptorq-vs-peers.md): 1.12x to 1.47x the best peer on K from 101 to 50000; at K = 10, 1.08x
  to 1.14x on decode and 3% to 7% behind nanorq on the encoder, explained there. Plans of 2.8 KB to 11 MB, no heap.
- clang-tidy clean on win-debug's files and the peer lane's two. crd-check-* guards and crd-ci-platform pass on linux-gcc-debug.

## Next

FlexFEC and RED against libwebrtc's reader, writer and splitter, with their budgets; then the row's remaining audits.
