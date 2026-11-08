# 2026-11-08 — NET.3: the check pass and decode into storage

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3). Board: [serializers](../bench/2026-11-08-net-3-serializers.md#decode-into-storage-and-the-check-pass-later-the-same-day).
Design: [S26 decoding](../design/network-foundation.md#wire-formats).

## CI first

Nothing new to fix. Nothing has completed since the last batch: 42423776692 and 42416903705 are queued, and the eight
failed jobs of 42413677756 (d7b506c8, still running) are older than 8d5c9e63, 2e5c900a and de175f3f/4b6a4a13.

## What landed

- hesap-net: `bounded_valid` and `smallest_three_valid`, the decode's verdict without the value. The quaternion check
  and `smallest_three_decode_one` read the same `smallest_three_stored` arithmetic.
- The generator emits a `check` on every list element and struct codec (a message has none). It differs from `decode`
  only in its quantised fields, and it forwards to `decode` when the two would read the same. `wire_take_list` runs it.
- `WireList::into(storage)`: a decode fills the storage in one pass, validating as it goes, keys included. The storage
  stays with the list across decodes and through struct fields (generated `keep`, `wire_fresh`). A list longer than its
  storage is viewed.
- wire_test gains minor 3: `Shelf` and `Shelved`, a struct field that holds a list.
- The generated fuzzers check that every element codec's `check` and `decode` agree in verdict and end position. A
  decode into storage must match the plain decode in verdict and message. New seeds are in the corpora. The
  session-control corpus also gains the 27 seeds that cdcdf807 changed without regenerating it.
- The board gives crd-net's lists storage and adds a "lists viewed" arm.

## Evidence

- Snapshot decode is 1.08x and 1.04x of serialize's time over two runs, down from 1.78x. The viewed form is 1.45x.
  Values are bit-identical to serialize's and nothing is allocated. Chat decodes at 0.64x to 0.68x of serialize's.
- linux-gcc-debug: `crd-net-tests` (322,137 assertions), `crd-hesap-net-tests [quantize]` (1,478,786), the net fuzz
  corpora, the generator's drift check, `crd-ceridc-tests [model]`. linux-gcc-release: the same tests and the board.
  test-wire-schemas.py: 29 tests.
- libFuzzer for 90 s each: wire-delta-test (3.5 M executions) and session-control (37 M) are clean. wire-test and
  delta-stream stopped on wild jumps at a different place each run (7 runs), and none of the inputs replays (50 runs
  each, both builds). With the same `-seed=7 -runs=150000`, two delta-stream runs stopped on different units. The
  20-round TSan host check run next faulted in the Cerid-free controls (control-plain: `=== WRONG lane walk`; a TSan
  SEGV). So it is this host's known fault ([host-misexecutes]), not settled: a hosted libFuzzer run of these two
  targets is the check.
- Mutations were caught. A quaternion check without its remainder fails `[quantize]` and the fuzzer's storage oracle
  (that input replays). A storage pass that skips key order fails test_delta.cpp.
- Strict clang-tidy 20 is clean on quantize.hpp, wire.hpp and wire_quantized.hpp (through their units), and on the
  tests and fuzz targets.

## Next

- The last 4% to 8% of snapshot decode, starting with the reader's per-field word load.
- Delta encode and apply cost: 5.2x and 3.2x a whole message.
- FlatBuffers and Cap'n Proto on the board. Then NET.3 closes.

[host-misexecutes]: 2026-10-11-diag-10a-container-overflow-control.md
