# 2026-11-07: NET.3, baseline deltas

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3) (stays Partial). Track C batch 115. Design:
[S28](../design/network-foundation.md#baseline-deltas). The GPL and LGPL rewrite is another agent's (operator note); this batch
did not touch it.

## CI first

Nothing to fix. No completed run contains 27fa4eea..e7f48539: 42386560979 is pending, 42380994537 and 42367379177 queued, the
rest cancelled.

## What landed

- `delta.hpp`, `delta.cpp` (`crd-net`, capability `network.admission`): `delta_encode` and `delta_apply` over encoded bytes,
  one interpreter walking generated tables; `BaselineStore`, `DeltaWriter`, `DeltaReader`.
- The schema language: `delta` on a message, `delta_group` on a uint, range, bounded, varuint or varsint, `delta_key` on a list.
  Each joins the layout digest. A keyed list's order is now the encoder's and decoder's rule everywhere (`KeyOrder`).
- `MessageDesc` bounds a delta message's frame body; `write_frame` and `decode_frame` refuse one. `session_control` gains
  `BaselineAck` at minor 1. `containers.wire-bits` gains `remaining` and `claim`, for a body nested in its own output.

## Evidence

- The reference interpreter computes on values and shares no code with the C++: 48 deltas with their bytes and 240 damaged ones
  with their verdict and rebuilt message, at every revision (`delta_test.json`).
- `crd-net-tests`: the whole target (274,739 assertions in 88 cases) on linux-gcc-debug and linux-gcc-asan, `[delta]` on
  linux-clang-tsan, `[delta],[wire]` (52,484 in 42) on MSVC win-debug. They spell each form's exact bits and each refusal, an output too small
  at every size, and a path with loss up to 70%, repeats and reordering: everything delivered is what was written, and deltas
  flow once it clears.
- libFuzzer on linux-clang-fuzz, 120 s each on the final sources: `crd-fuzz-wire-delta-test` 7,503,624 and
  `crd-fuzz-net-delta-stream` 126,278 executions, no artifact; an earlier round's grown inputs and the committed wire and admission
  corpora replay clean on linux-gcc-asan.
- `test-wire-schemas.py`: every new refusal. Strict clang-tidy 20 clean on the new files. `crd-no-` and `crd-check-` guards pass.
- 410 source mutations of `delta.cpp` (10 did not compile). The first round's real gaps are now tests: exact-fit rooms, a store
  wrapping over slot zero, a one-byte message, the whole-or-delta boundary, a span's distance, half-span sequences, the largest
  message. Final: 353 caught (9 only under ASan), 47 equivalent (initial values always overwritten, alignment, bounds no input
  reaches). A test found `delta_expand_sequence` wrapping at the top of 64 bits, fixed; checks no input reaches are gone.

## Next

NET.3: the plaintext integrity hash on non-delta messages and the state-hash exchange; padding policies; dictionary compression
for reliable streams and entropy coding for snapshots through crd-compression, with ratio and output caps and S6's charge for what
a delta or a compressed frame expands to; the budget board against the best serializers.
