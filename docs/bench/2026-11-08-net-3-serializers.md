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
  time for its keys (commit 8d2367f2).
- **Deltas cost CPU.** Encoding a delta takes 5.2x a whole message's encode and applying one 3.2x its decode: S28's
  layout walk interprets every field from descriptors.

## Decode into storage and the check pass (later the same day)

Same machine and tree, two runs of `--rounds 25` ([session](../sessions/2026-11-08-net-3-one-pass-decode.md)). The
crd-net arm now gives its lists storage (`WireList::into`): each element is decoded once as it is validated. A sixth
arm, "crd-net, lists viewed", is the same codec without storage: its validating pass runs the generated `check`, which
rebuilds no float or quaternion, and the walk decodes.

| Message | crd-net, into storage | lists viewed | serialize | crd-net / serialize |
|---|---|---|---|---|
| snapshot, decode ns (runs 1, 2) | 1203.9, 1137.6 | 1749.3, 1676.6 | 1116.8, 1092.3 | 1.08x, 1.04x |
| snapshot, encode ns | 1506.4, 1457.5 | 1533.1, 1455.4 | 1897.9, 1836.5 | 0.79x, 0.79x |
| chat, decode ns | 57.7, 53.5 | 64.6, 62.1 | 84.5, 84.1 | 0.68x, 0.64x |
| input command, decode ns | 15.3, 12.4 | 16.9, 13.7 | 13.1, 10.7 | 1.17x, 1.16x |

- The snapshot decode went from 1.78x serialize's to 1.04x to 1.08x, every value still equal to serialize's bit for
  bit, nothing allocated. The check pass alone takes the viewed form from 1.78x (1936 ns) to 1.45x.
- The last 4% to 8% is not yet attributed. The first candidate is the reader: each field reloads an unaligned word at
  its bit position and checks the bound twice, where serialize shifts a scratch word it refills once per 32 bits.

## Deltas as word runs (later the same day)

Same machine and tree, two runs of `--rounds 25` ([session](../sessions/2026-11-08-net-3-delta-runs.md)). Before:
the profile (`perf record`, 15 rounds) put 21% of the board in the delta reader's out-of-line `take` and most of the
rest in per-field dispatch: an unchanged entity was compared, and a changed one rebuilt, field by field. Now each table
row names its run of fixed-size siblings (S28), compared and copied as 64-bit words, and a plain value is read once.

| Baseline age | encode ns before | encode ns (runs 1, 2) | apply ns before | apply ns (runs 1, 2) |
|---|---|---|---|---|
| 1 tick | 7659 | 3574.0, 3631.4 | 6142 | 3182.1, 3278.3 |
| 6 ticks | 10723 | 6414.7, 6455.8 | 7944 | 5500.4, 5542.4 |

- At one tick a delta encode now takes 2.4x a whole snapshot's encode (1494.5 ns and 1514.5 ns in the same runs), from
  5.2x, and an apply 2.8x its decode (1140.4 ns, 1135.8 ns), from 5.4x against today's decode. Bytes are unchanged.
- The gap against the floor: an encode reads two encodings and an apply reads two and writes one, so about two decodes'
  worth of bit reads each. What is left above it is per changed field: a fresh scan of the run's remaining words after
  each difference, and the folded differences as three-bit varint groups read and written one group at a time.

## FlatBuffers and Cap'n Proto (later the same day)

Same machine and tree, two runs of `--rounds 25` ([session](../sessions/2026-11-08-net-3-zero-copy-peers.md)). Two
arms join, each through the C++ its own schema compiler generates at build time (`board.fbs`, `board.capnp`; flatc and
capnp are built from the pinned sources as separate projects): FlatBuffers 25.12.19, an entity as a struct inline in
its vector, read after the library's `Verifier`; Cap'n Proto 1.5.0 in its packed form, through kj's array streams with
the unit's own first segment and read scratch. Both copy into the plain message; neither allocates.

| Message | Arm | bytes | encode ns (runs 1, 2) | decode ns (runs 1, 2) |
|---|---|---|---|---|
| snapshot, 64 entities | crd-net | 1235.9 | 1617.4, 1657.9 | 1208.1, 1211.0 |
| | serialize | 1236.9 | 2017.7, 1970.7 | 1149.1, 1117.5 |
| | FlatBuffers | 3100.0 | 288.4, 256.3 | 249.3, 249.8 |
| | Cap'n Proto, packed | 3050.5 | 1556.4, 1536.0 | 2680.2, 2614.4 |
| input command | crd-net | 11.0 | 21.1, 22.3 | 18.4, 16.9 |
| | FlatBuffers | 40.0 | 35.0, 29.6 | 8.9, 8.6 |
| | Cap'n Proto, packed | 21.7 | 87.7, 79.8 | 67.1, 67.1 |
| chat | crd-net | 83.4 | 64.3, 64.3 | 57.6, 55.1 |
| | FlatBuffers | 124.2 | 61.9, 59.6 | 34.2, 34.1 |
| | Cap'n Proto, packed | 99.2 | 107.4, 110.5 | 132.8, 133.3 |

- **Bytes.** crd-net's snapshot is 2.5x smaller than either (FlatBuffers' 48-byte entity, Cap'n Proto's packed
  words), its command 3.6x and 2.0x, its chat 1.5x and 1.2x. Packing buys Cap'n Proto only 2% on snapshots: an
  entity's floats have no zero bytes to drop.
- **CPU.** FlatBuffers is the fastest arm on snapshots, 0.16x crd-net's encode and 0.21x its decode, by sending the
  application's layout at 2.5x the bytes (zpp_bits' trade, with offsets). Cap'n Proto's packed form is crd-net's
  encode cost and 2.2x its decode, at 2.5x the bytes; its command costs 4x crd-net's both ways.
- **Damage.** Cap'n Proto refuses a snapshot cut or extended by a byte (the arm also refuses bytes left after the
  message). FlatBuffers' verifier does not refuse the pair: a buffer records no length of its own (only the size-prefixed form
  does), so a byte after the message is never refused.
- The host was in use: crd-net's snapshot decode moved to 1.05x and 1.08x serialize's in these runs, and its command
  cells by up to 1.5x, as before.
