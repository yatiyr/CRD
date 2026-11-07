# 2026-11-08 — NET.3: the serializer budget board

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET.3](../ROADMAP.md#slice-net.3). Board: [serializers](../bench/2026-11-08-net-3-serializers.md).

## CI first

- linux-gcc-relwithdebinfo in dispatch 42413677756 (d7b506c8) failed two ways. Its `-Warray-bounds` links are older
  than their fix (8d5c9e63). New: GCC's link-time `-Wnull-dereference` in `crd-fuzz-net-coded-frames`, because
  `write_coded_body` read a coding's model and coder unchecked. Fixed in de175f3f and 4b6a4a13: the header now follows
  the model as the reader's admission does (no model, `write_frame`'s frame; a model without a coder, the coded header
  and a stored body), `write_frame` encodes into its output when no coder holds a scratch, tested through both writers
  and the session's reader, the shape choice mutation-checked. Links clean on relwithdebinfo and release here.
- The same run's linux-gcc-shipping and -release failures are the array-bounds links fixed in 8d5c9e63; the clang lanes'
  are the lambda capture fixed in 2e5c900a. win-tidy was still running.

## What landed

- `crd-net-serialize-board` (tests/foundation/net/board, `CRD_BUILD_SERIALIZE_VS_PEERS`, on public-checks): crd-net's
  generated codec against serialize with networked-physics' smallest three, bitsery, zpp_bits and protobuf's format
  through protozero, on a 64-entity snapshot stream, input commands and chat; bytes, encode and decode time,
  allocations, damage refusal; baseline deltas over the same stream. Pinned peers: bitsery 5.2.5, zpp_bits 4.7.6,
  protozero 1.8.2. serialize.h and the cut quaternion structs are one root target shared with the quantisation peers.
- The board found crd-net's snapshot decode at 7.7x serialize's. hesap-net gained `smallest_three_encode_one` and
  `smallest_three_decode_one` (held to every level's batch word for word); `wire_take_list` checks keys on its
  validating pass; the small field readers are inlined. Snapshot encode 2865 to 1471 ns (serialize 1862), decode 8670
  to 1936 ns (serialize 1088).

## Evidence

- The board passes twice at 25 rounds: every arm within its law, crd-net's values equal to serialize's bit for bit, no
  allocation by crd-net. Its smoke passes on linux-gcc-public-checks with the quantisation peer tests.
- `crd-net-tests` (320,911 assertions) and `crd-hesap-net-tests [quantize]` (1,408,994) on linux-gcc-debug; the
  related CTest selection (208) and the generator's tests and drift check.
- Strict clang-tidy 20 clean on the changed sources and headers after 813a6d45, which also fixed a link-time
  `-Wnull-dereference` the inlining exposed in test_delta.cpp; the net tests and every net fuzz target link on
  linux-gcc-release, -relwithdebinfo and -shipping. Complete-tier dispatch 42423776692 runs at 813a6d45.

## Next

NET.3's decode within serialize's cost: a validating pass that checks codes without rebuilding floats and
quaternions; delta encode and apply (5.2x and 3.2x a whole message); FlatBuffers and Cap'n Proto on the board with their
schema compilers. Then NET.3 closes.
