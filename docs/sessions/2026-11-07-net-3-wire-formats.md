# 2026-11-07: NET.3, generated bit-packed wire formats

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3) (now Partial). Track C batch 112. Design:
[S26](../design/network-foundation.md#wire-formats).

## CI first

Nothing new from this track. Dispatch 42360379686 (8e86cf8f): `crd-fuzz-hesap-motion-corpus` passes on linux-clang-arm64-debug
and -asan (fix 57c19b1e); linux-gcc-public-checks passes 9,861 of 9,861, the first hosted run of the netem, nanorq, raptorq
crate and libwebrtc FEC peers. HESAP-NET now waits only on the NEON body of `sum`; 42367379177 (d4a1b48f) is queued.

## What landed

- `containers.wire-bits` (`wire_bits.hpp`): most significant bit first, a writer that reports running out of room and keeps
  measuring, a reader whose takes consume nothing when short.
- `network.wire-formats`: the schema model (`cerid-wire-schema/1`), `scripts/gen-wire-schemas.py`, `crd/net/wire.hpp` and
  `crd/net/wire_quantized.hpp`, the first real schema `session_control` (revision announce, schema, capability and revocation
  presentation with crd-crypto's bounds, generations, state hash, going away).
- Types: bool, uint, range, varuint, varsint, enum, f32, f64, hesap-net's bounded float and quaternion, bytes, text, list;
  optional fields; plain and extensible structs. One message has one encoding.
- Revisions: no per-message cost; a minor only appends; each minor's layout digest is recorded and checked.
- crd-net does not link crd-hesap-net, which is not a packaged library; a quantised schema's target links `crd-net-quantized`,
  whose header stays out of crd-net's installed and publicly checked headers.

## Evidence

- The generator's reference interpreter shares no code with the emitted C++: 12 test cases, 7,973 assertions, including
  bytes and verdicts for every message at every minor, older and newer writers, on damaged input. Linux GCC debug and ASan,
  Clang TSan, MSVC win-debug. `crd-containers-tests [wire-bits]`: every width at every bit offset against a model.
- `test-wire-schemas.py`: 21 cases, every refusal, the history rule, bounds against the reference writer.
- libFuzzer on linux-clang-fuzz, 120 s each: `crd-fuzz-wire-session-control` 52,051,390 and `crd-fuzz-wire-test` 13,766,836
  executions, no artifact; their grown corpora replay clean under ASan.
- 57 source mutations of the codec and the generator's emission. The first round had real survivors: a varsint inside its
  zigzag but outside its bound, an extensible length of two bytes, an encoder's signed bound, a dirty decode target and zero
  groups past the value's width. Each got a test, and all 57 are now caught.
- A count pre-check that added no bound was removed: every element takes a bit, so the walk is linear anyway.

## Next

NET.3: frames, admission S6 and S7 over `authorize_operation`, quotas, reassembly and backpressure; baseline deltas; dictionary
and entropy coding through crd-compression; padding policies; the plaintext integrity hash; the budget board.
