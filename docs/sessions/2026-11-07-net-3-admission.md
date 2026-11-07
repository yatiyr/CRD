# 2026-11-07: NET.3, frames, admission S6 and S7, quotas and reassembly

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3) (stays Partial). Track C batch 114. Design:
[S27](../design/network-foundation.md#frames).

## CI first

Nothing new from this track. No completed run contains 3f34263b or 3c142043 yet (42378527799 pending, 42367379177 queued,
42360379686 still running with no failed job). The strict tidy named after batch 112 was run here: `bugprone-branch-clone` on
the generated `enum_known`; the generator now groups labels by revision, regenerated. `test-repository-tools.py`, run here,
found batch 113's netem step calling `apt-get` directly with no step timeout, which fails `crd-repository-tools`; fixed.

## What landed

- `frame.hpp`: a route named once per carrier (flat channel number and form), and headers for stream, whole, listed and
  fragment. The length sits before the object, so a newer writer's unknown message can be stepped over.
- `quota.hpp`: virtual-scheduling gates in integer nanoseconds (one 64-bit word, per session or shared atomically), gauges,
  `PrincipalQuota`, priority headroom, `SendBudget`.
- `admission.hpp`: `Admission::read_frame` and `admit` run S6 (spending nothing until all holds) and S7 over
  `authorize_operation`. `bind()` holds each signed artefact to its generated identifier, kind and major. Verdicts map to
  Deliver, Continue, Wait, Skip or Close by channel reliability, and each is counted.
- `reassembly.hpp`: `StreamAssembler` (zero copy when whole, one buffer of the declared length otherwise, a 20-byte header
  carry) and `DatagramAssembler` (in any order, fixed slots and bitmaps, timeouts, eviction, latest wins, a 64-sequence window).
- `crd-net` now links `crd-crypto` and `crd-memory`; `wire::find_message` searches the generated tables.

## Evidence

- `crd-net-tests`: 58 cases, 230,229 assertions, on linux-gcc-debug, linux-gcc-asan and MSVC win-debug; the 29 new ones on
  linux-clang-tsan, the shared gates and gauges under four threads among them. The `crd-no-` and `crd-check-` guards pass.
- Strict clang-tidy 20 clean on all new files and the generated header (linux-gcc-debug database).
- `crd-fuzz-net-admission`: a restated layout and rule decide every verdict and frame; splits read as whole does; every budget
  comes back. 1,011,405 libFuzzer executions on the final sources (592,000 before), no artifact; 876 grown inputs replay
  clean under ASan.
- Source mutations: the first round of 158 had 14 survivors. One was equivalent and dropped, one exposed a redundant check
  (`left > max` on Whole, removed), the rest each got a test. Final round of 155: 154 caught, one did not compile (`find_slot`
  ignoring the sequence).
- Hostile path: loss 0, 5 and 30%, 10% duplication, a reorder window of 24 datagrams, 8 seeds each: each message delivered
  intact at most once, and all of them with no loss; every quota returns to zero.

## Next

NET.3: baseline deltas; dictionary and entropy coding through crd-compression with ratio and output caps (S6's decompression
quota); padding policies; the plaintext integrity hash and state-hash exchange; the budget board.
