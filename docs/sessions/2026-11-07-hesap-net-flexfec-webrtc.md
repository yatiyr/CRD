# 2026-11-07: FlexFEC and RED against libwebrtc's own files

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 109.

## CI first

Push run 42343637429 (229b06b2, still running): `crd-fuzz-hesap-motion-corpus` failed on linux-clang-arm64-debug and -asan at
fuzz_motion.cpp:369. Clang on ARM64 fuses `a + b * c` even at -O0, and fuzz targets did not take the engine's floating-point
contract. Reproduced on x64 with `-ffp-contract=on -mfma`, same line; `crd-fuzz-harness` now carries the contract (57c19b1e).
No hosted run has confirmed the fix. That run is also the motion body's first NEON evidence: `crd-hesap-net-kernels-at-neon` has
the motion cases on linux-gcc-arm64-debug, win-arm64-debug and linux-clang-arm64-asan, and only that fuzz target failed on those lanes.

## What landed

- `crd-webrtc-fec-peer`: the pin grows by 35 files of branch-heads/8147, verbatim. Shimmed: Abseil's small pieces, and headers
  the files name for two constants or for members the codec never reads.
- `crd-hesap-flexfec-webrtc-tests` (7 cases, 1.8 million assertions, on linux-gcc-public-checks).
- `crd-hesap-flexfec-board` and its [board](../bench/2026-11-07-hesap-net-flexfec-vs-webrtc.md).
- `flexfec_parse` and the mask writer without bit loops: a refusal still writes nothing, and blocks past the stream count now stay
  as they were. The board found the gap: 141 ns against 4.5 ns.

## What libwebrtc's files showed

- `CreateFlexfec` still builds the draft-03 reader and writer; the RFC 8627 pair is in the tree beside them.
- The RFC 8627 reader needs its streams from the CSRC list, which this branch's codec never parses, so its `DecodeFec` refuses every
  RFC 8627 packet as shipped. The oracle's `CsrcReader` names the stream, as an RTP layer would.
- It reads only flexible masks, so retransmissions and rows and columns stay with `test_flexfec.cpp`.

## Evidence

- Reader: 300,000 hostile headers of 0 to 15 streams: same verdict, sequence bases, every mask bit, header size and recovery
  fields. One difference, kept: hesap refuses an empty mask, which libwebrtc reads and later drops.
- Writer: 6,000 masks of 16 and 48 bits through its generator and writer, byte for byte, except its 48-bit form for a mask a
  15-bit field holds, which is 4 bytes longer than hesap's shortest and otherwise equal. 3,000 headers of up to 15 streams.
- Codec: 4,000 windows on its own masks, both mask types, unequal protection, gaps, the wrap and 20% loss. Each decoder on the
  other's repairs and on its own reaches the same packets, byte for byte. 2,000 windows with hesap's masks of up to 110 bits.
- RED: 300,000 hostile payloads against `SplitRed`: same verdict and blocks, apart from NetEq's dropping of empty blocks and its limit
  of 32. 50,000 `red_encode` payloads read back.
- 20 teeth in flexfec.cpp and red.cpp, all caught by this oracle alone.
- After the parser's rewrite: `crd-hesap-net-tests` clean on linux-gcc-debug, `[flexfec]` on linux-clang-fuzz and win-debug, 1.14 million
  libFuzzer executions clean under ASan and UBSan, linux-gcc-shipping compiles. clang-tidy clean on win-debug's and the peer lane's files.
- The estimator peers, which share the shim, rebuilt: `crd-hesap-estimators-webrtc-tests` passes, its board's smoke run passes.

## Next

Close the row's audits: the first hosted runs of the serialize, vectorial, KDIS, netem, nanorq, raptorq crate and FEC peers, and the
avx512 throughput on a real AVX-512 machine (one machine for one run, deleted after).
