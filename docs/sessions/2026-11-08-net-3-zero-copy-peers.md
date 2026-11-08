# 2026-11-08 — NET.3: FlatBuffers and Cap'n Proto on the serializer board

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3). Board: [serializers](../bench/2026-11-08-net-3-serializers.md#flatbuffers-and-capn-proto-later-the-same-day).

## CI first

Nothing new to fix: no run has completed since the last batch. Dispatch 42430818018 (378eed04) is queued; the failures
in 42413677756 and 42416903705 are older than their fixes (8d5c9e63, 2e5c900a, 20a1cfcd).

## What landed

- Pins: FlatBuffers 25.12.19 (Apache-2.0) and Cap'n Proto 1.5.0 (MIT), commit-addressed archives with SHA-256 and size,
  gated on `CRD_BUILD_SERIALIZE_VS_PEERS` (the public-checks lane), in the license manifest.
- tests/foundation/net/board: flatc and capnp (with static kj and capnp, no OpenSSL, zlib or fibers) built by their own
  CMake as separate projects; `board.fbs` and `board.capnp` mirror `board.proto`; their C++ is generated into the build
  tree. `peer_flatbuffers.cpp` (struct entities, read after the `Verifier`) and `peer_capnproto.cpp` (the packed form,
  kj array streams, the unit's own first segment and read scratch, exceptions turned into refusals, trailing bytes
  refused) join the board as two more arms held to the source exactly. The board's scratch buffer is aligned (the
  verifier refuses a misaligned one).

## Evidence

- linux-gcc-release (WSL, i9-14900K, host in use): the board builds from a clean peer prefix; `--quick` and two runs
  of `--rounds 25` pass: every arm agrees with the source, crd-net equals serialize bit for bit, nothing allocates.
- crd-net is 2.5x smaller than both on snapshots. FlatBuffers encodes a snapshot in 0.16x and decodes it in 0.21x
  crd-net's time at 2.5x the bytes; Cap'n Proto packed matches crd-net's encode and takes 2.2x its decode.
- Cap'n Proto refuses a snapshot cut or extended by a byte; FlatBuffers does not (no length of its own).

## Next

- NET.3's remaining clauses: delta encode and apply (one scan per run for all its differing fields, a one-group path
  for folded differences) and the last 4% to 8% of snapshot decode.
