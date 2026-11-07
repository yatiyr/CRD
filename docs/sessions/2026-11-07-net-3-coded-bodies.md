# 2026-11-07: NET.3, coded bodies

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3) (stays Partial). Track C batch 117. Design: [S32](../design/network-foundation.md#body-coding).
The GPL and LGPL rewrite is another agent's; this batch did not touch it.

## CI first

- win-tidy in 42367379177 (d4a1b48f) failed one name of this track, `kSumColumns` in `erasure_kernels.cpp`: fixed first (fceb19a1).
- That run's win-asan passed `Capabilities: verification and revocation run on several threads at once`, the first hosted evidence
  on batch 106's open item. Nothing has failed it since 42319502866.
- 42406667273 (287ea859) was cancelled unread; no completed run holds 27fa4eea or later yet. This batch dispatches a complete run.

## A defect found on the way, fixed

`ZstdMessageCoder::compress` running out of room left zstd's session open, and every later compress on that context then refused
its dictionary and looked like no gain: a coder would silently stop compressing for good after the first message that did not pay.
`crd-fuzz-compression-messages` found it on its first replay; the context now resets its session first, with a regression test.

## What landed

- `entropy.hpp`: a static rANS coder for small messages, 12-bit frequencies in four contexts, one 3-byte state, one encoding per
  message. `EntropyTrainer`, `EntropyModel`. Choice measured on simulated deltas of 56 bytes: order-0 0.71, full order-1 0.70, four
  contexts 0.62, zstd with a 4 KiB dictionary 1.08.
- `zstd.hpp`: `ZstdDictionary`, `ZstdMessageCoder` with static workspaces and magicless frames, `zstd_train_dictionary`.
- `coding.hpp`: `BodyModel`, `BodyCoder`, `BodyCoding`, `expansion_holds()`. `CodecDesc` and `codec_at()` in `wire.hpp`.
- Frames: `[expanded?]`; `write_coded_body`, a coding `write_frame`; `BadExpansion`. S6 charges a coded frame what it expands to and
  reserves its room with reassembly's. `Admission::read_delta` holds a closed gate before a delta's rebuild and charges it after.
- Generator: a channel's `models`, part of its revision's digest, refused on Control, Transaction and Media channels and transport
  schemas. `tests/foundation/net/schemas/coding_test.json`.

## Evidence

- `crd-net-tests`, all of it: 320,876 assertions in 127 cases on linux-gcc-debug and linux-gcc-asan, 320,877 on MSVC win-debug;
  `[coding]`, `[admission]` and `[stream]` on linux-clang-tsan. `crd-compression-tests`: 169,053 in 37 cases on the same lanes.
- libFuzzer on linux-clang-fuzz, 120 s each on the final sources: `crd-fuzz-net-coded-frames` 235,504 and
  `crd-fuzz-compression-messages` 628,028 executions, no artifact. Committed corpora replay clean on every lane above, with the
  admission, delta-stream and wire-delta corpora.
- Strict clang-tidy 20 clean on every changed source and new test. CTest guards (`crd-check-*`, `crd-no-*`), the wire-schema and
  entropy-vector drift tests and `test-wire-schemas.py` (the model refusals and digests) pass.
- Mutations of the changed functions: a first round of 56, 39 caught, 12 survived, 5 did not compile. Three survivors were
  redundant checks, now gone (a coder's size floor, `coded != 0` in `expansion_holds`, the `min` of the two revisions, which the
  reader's own table already applies), and one only bounds how long a coder tries. The rest became tests: a decode from below the state's range, the frame descriptor, a
  writer's room at each boundary, a delta's ratio held by its reader, a whole delta's charge. A second round of those 13: 12 caught;
  zstd's content-size flag survives, equivalent on this path, and stays explicit.

## Next

NET.3's budget board against the best serializers, and the command that trains a schema's models.
