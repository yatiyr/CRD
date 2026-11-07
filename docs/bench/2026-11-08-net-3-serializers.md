# 2026-11-08 — NET.3: the serializer budget board

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3). Session: [Serializer board](../sessions/2026-11-08-net-3-serializer-board.md).

## Machine and arms

Intel Core i9-14900K (AVX2), WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_QUANTIZE_VS_PEERS=ON -DCRD_BUILD_SERIALIZE_VS_PEERS=ON`; the host was in use, so both runs are shown.
`crd-net-serialize-board --rounds 25` (tests/foundation/net/board): 600 ticks of a 64-entity world at 60 Hz, 1,200 input
commands and 150 chat messages (schema `serialize_board.json`, protobuf form `board.proto`), the best of 25 interleaved
rounds after an untimed warm-up. Every arm starts from the application's plain message and ends in it; every decode
refuses malformed bytes (each refused a snapshot cut or extended by a byte). The peers, as released and built without
link-time optimisation: mas-bandwidth/serialize 2.1.3 with networked-physics' smallest three (precomputed compressed
floats, 32-bit tick), bitsery 5.2.5 (bit packing, value ranges, four ranged quaternion components at ten bits), zpp_bits
4.7.6 (natural types) and protozero 1.8.2 writing protobuf's format (proto3 defaults left out). The board fails if any
arm's values leave its law, if crd-net's differ from serialize's in any bit, or if crd-net allocates.

## Per message (run 2)

Run 1 agrees within 4% on the snapshot and chat cells; the command cells, a few nanoseconds each, moved by up to 1.5x
between the runs on the busy host (crd-net 18.0 and 18.3 ns in run 1).

| Message | Arm | bytes | encode ns | decode ns | allocations |
|---|---|---|---|---|---|
| snapshot, 64 entities | crd-net | 1235.9 | 1471 | 1936 | 0 |
| | serialize | 1236.9 | 1862 | 1088 | 0 |
| | bitsery | 1324.9 | 1609 | 2709 | 0 |
| | zpp_bits | 3080.0 | 401 | 402 | 0 |
| | protobuf (protozero) | 3575.3 | 1196 | 1341 | 0 |
| input command | crd-net | 11.0 | 14.9 | 12.1 | 0 |
| | serialize | 12.0 | 11.4 | 9.6 | 0 |
| | bitsery | 12.0 | 9.5 | 8.7 | 0 |
| | zpp_bits | 16.0 | 6.7 | 3.0 | 0 |
| | protobuf (protozero) | 21.9 | 16.2 | 17.4 | 0 |
| chat | crd-net | 83.4 | 54.8 | 53.2 | 0 |
| | serialize | 83.4 | 48.6 | 86.4 | 0 |
| | bitsery | 83.0 | 67.6 | 64.9 | 0 |
| | zpp_bits | 85.6 | 21.9 | 23.2 | 0 |
| | protobuf (protozero) | 88.2 | 29.0 | 38.4 | 0 |

## Baseline deltas over the same stream (S28; no peer has them)

| Baseline age | whole bytes | delta bytes | delta / whole | encode ns | apply ns |
|---|---|---|---|---|---|
| 1 tick | 1235.9 | 233.4 | 0.189 | 7659 | 6142 |
| 6 ticks | 1236.0 | 585.3 | 0.474 | 10723 | 7944 |

## Reading

- **Bytes.** crd-net is the smallest on snapshots and commands and within half a byte of bitsery on chat: a byte under
  serialize's (the same laws, a varint tick), 7% under bitsery's snapshots (its four-component rotations), 2.5x under
  zpp_bits and 2.9x under protobuf. A delta against the newest acknowledged tick sends 19% of the message.
- **Encode.** crd-net beats serialize and bitsery on snapshots (0.79x and 0.91x of their time) and lies between them on
  chat. zpp_bits is 3.7x faster at 2.5x the bytes: it copies natural types and checks nothing but room.
- **Decode, the gap.** Snapshots decode at 1.78x serialize's time. A list is validated in full before any element is
  handed out (S7: nothing executes on a malformed message), then walked, so each entity's fields are read twice;
  serialize reads once and leaves the caller a half-filled struct on failure. The validating pass also rebuilds every
  float and quaternion it then throws away. Commands decode at 1.26x serialize's, chat at 0.62x.
- **Before this batch** the same board measured crd-net's snapshot encode at 2865 ns and decode at 8670 ns: a
  quaternion field went through hesap-net's dispatched batch with a count of one, and a keyed list was walked a third
  time for its keys (commit 8e23fb27).
- **Deltas cost CPU.** Encoding a delta takes 5.2x a whole message's encode and applying one 3.2x its decode: S28's
  layout walk interprets every field from descriptors.
