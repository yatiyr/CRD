# 2026-11-04: HESAP-NET, FlexFEC parity of RFC 8627 (Track C batch 97)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (Partial). CI first: nothing to audit; dispatches 41778055691
(304f0432) and 41763564869 and push run 41777851178 are still queued or pending.

## What landed

- RFC 8627 read in full from the RFC Editor (rfc8627.txt, SHA-256 5ce74660..., 2299 lines); register entry
  `fec.flexfec` is now implemented, its note naming this read.
- `hesap.erasure` gains `add` (GF(2^8) addition, a vector XOR at portable, SSE2, AVX2, AVX-512 and NEON) with its own
  known-answer self-check.
- crd/hesap/comms/flexfec.hpp (capability `numerics.erasure-codes`): `flexfec_parse` (Figures 10 to 15: masks of 15,
  46 and 110 bits chained by k = 1, L columns by D rows, retransmissions, up to 15 streams), `flexfec_protected`,
  `flexfec_encode` (§6.2's bit strings, checking the packets are exactly the named set), `flexfec_recover` (§6.3.2 and
  6.3.3) and `flexfec_recover_window`, §6.3.4's iterative decoding as a peeling over masks, rows and columns alike.
  Nothing allocates; every refusal writes nothing. §6.3.2's "first 64 bits" would XOR the sequence number into the
  length recovery field; both directions follow §6.2 and Figure 12, which skip it (recorded in the register note).
- `crd-fuzz-hesap-flexfec`: a parser restated bit by bit, injected encode faults, §6.2's bit string built literally,
  every member recovered from the others, and the window decoder against a peeling of index sets; 25 seeds.

## Evidence

- win-debug: crd-hesap-comms-tests 47 cases (41612 assertions), the 27 kernel-level CTests (portable to avx512), the
  corpus replay (25 inputs). Teeth: halving the recovery XOR fails 5 FlexFEC cases; in the fuzz replay, a window
  decoder that also recovers with two packets missing, a dropped duplicate check and a misread k bit each fail.
  The `crd-no-`/`crd-check` guards pass (crd-check-capability-owners segfaulted inside Python once, passed rerun).
- linux-gcc-debug (WSL): the FlexFEC and erasure cases (1949 assertions) and 29 CTests (kernel arms and both
  corpora) pass, no warnings.
- linux-clang-fuzz (WSL), `fuzz.py run`: two 420 s campaigns at 2 jobs, 13.1M and 13.0M executions; each reported
  one finding (a jump into a data page; a UBSan pointer "subtraction" in `load16` that yields +1).
  Both pass replayed alone, and all 1349 grown inputs and artifacts pass in one process twice: this host's recorded
  misexecution under load, as in batch 95.

## Next

The ISA-L NASM `ec_encode_data` budget and the OpenFEC RFC 5510 oracle; FlexFEC against libwebrtc's RFC 8627
reader and writer (check it is not the draft-03 layout first) and its budget; RaptorQ (RFC 6330); the interleavers.
