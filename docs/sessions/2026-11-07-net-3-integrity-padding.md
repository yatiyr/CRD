# 2026-11-07: NET.3, the integrity hash, padding and state hashes; HESAP-NET closes

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET.3](../ROADMAP.md#slice-net.3) (stays Partial), [HESAP-NET](../ROADMAP.md#slice-hesap-net) (Done). Track C batch 116.
Design: [S29](../design/network-foundation.md#integrity-hash), [S30](../design/network-foundation.md#padding),
[S31](../design/network-foundation.md#state-hashes). The GPL and LGPL rewrite is another agent's (operator note); this batch did
not touch it.

## CI first

Nothing of this track failed. Run 42388007605 shows no failed job before its cancel. Dispatch 42367379177 (d4a1b48f), still running
other lanes, is the first run with `sum`'s NEON body: all five ARM64 jobs pass `crd-hesap-net-kernels-at-neon` with 0 tests failed,
which was HESAP-NET's last clause. It closes.

## A defect found on the way, fixed

A frame's length counted only its body, with the object between the length and the body. A reader stepping over a newer writer's
message with an operation bit skipped too little and lost the stream; both sides of the fuzz oracle restated the same rule, so it
never showed. The length now counts everything after itself. The regression test reads such a frame whole and a byte at a time.

## What landed

- `frame.hpp`, `admission.hpp`, `reassembly.hpp`: the hash in every form's header; `FrameShape`, `frame_shape()`, `frame_hash()`;
  `IntegrityFailed`; `RouteForm::Padded`, message zero as a stream's padding, `padded_size()`, `pad_datagram()`,
  `write_stream_padding()`, `BadPadding`, `charge_padding()`.
- `ChannelDesc::integrity`; the generator requires `padding_block` (0 or at least 16) and lets only Media turn the hash off.
- `state_hash.hpp`, `state_hash.cpp`: `StateHashExchange`. `crd-fuzz-net-state-hash`.
- `tests/foundation/net/admission_fixture.hpp` holds what the admission test files share.

## Evidence

- `crd-net-tests`, the whole target: 313,321 assertions in 108 cases on linux-gcc-debug and linux-gcc-asan, one more on MSVC win-debug; the
  frame, integrity, padding, state-hash and stream tags on linux-clang-tsan. They spell every header form's bytes, flip every bit of a
  hash and body whole and in pieces, pad every size up to 300 bytes to its exact multiple, and refuse each bad padding.
- libFuzzer on linux-clang-fuzz: `crd-fuzz-net-admission` 461,153 executions in 120 s (a restated layout, hash and padding; lives with
  flipped pieces), `crd-fuzz-net-state-hash` 4,424,565 in 60 s, no artifact. The committed corpora replay clean on linux-gcc-asan.
- `test-wire-schemas.py`: the new refusals. Strict clang-tidy 20 clean on the changed sources and new tests.
- 253 source mutations of the changed functions (last round: 221 caught, 25 survived, 7 did not compile; tests aimed at three of the survivors came after it and those three were not rerun). The first round's real gaps are now tests (exact padded sizes, room at every
  size, rings of odd sizes past their wrap, step zero after a restart, priority zero's exemption, a newer message at the channel's
  most) and two checks no input reached are gone. What survives is initial values always overwritten and equal-branch comparisons.

## Next

NET.3: dictionary compression for reliable streams and entropy coding for snapshots through crd-compression, with ratio and
output caps and S6's charge for what a delta or a compressed frame expands to; then the budget board against the best serializers.
