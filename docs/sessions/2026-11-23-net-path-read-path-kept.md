# 2026-11-23: NET-PATH, the path a packet came by is kept until that packet chose its path

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S75](../design/network-foundation.md#migration),
"IDs for waiting paths".

## What failed

Fuzz host-check 45084570688 (b119357b), `net-quic-connection`, container-overflow-off round 8602, exit 77: the oracle
`server.on_preferred_address() || server.path().socket == 0U` fired. Downloaded and replayed on HEAD it passed, because
27de8257 had pinned the over-paths server's ID limit to 2. Under the old limit of 4 it still failed on HEAD's engine.

The engine was wrong. A datagram from the client's path R carried a NEW_CONNECTION_ID. Its `address_paths()` gave R's
ID to a waiting path, and `drop_path()` emptied R's slot while `m_receive_path` still named it. `follow_packet()` then
read the emptied slot and left a waiting move on it. A later 4-tuple on the server's socket 1 was admitted into that
slot, and `follow_waiting()` moved the server there unvalidated, without `take_preferred()`. The same stale slot took
PATH_CHALLENGE and OBSERVED_ADDRESS state. A first fix that only cleared `m_receive_path` lost the move instead, and the
follow oracle caught that on committed input ddaef1b3.

## What landed

- `drop_path()`: from the packet's admission until `follow_packet()` chose its path (`m_deciding`), a drop of the packet's
  own path waits in `m_drop_after`. `decided()` then does it, and does nothing when the packet moved the connection there.
  This covers `address_paths()` at admission, on NEW_CONNECTION_ID and on PATH_RESPONSE, and a client's fallback.
- A path forgotten after that leaves the rest of its datagram (held packets) from a path not known, so the next 1-RTT
  packet admits it again.
- Connection fuzzer: an over-paths script that opens with 0xF3 has the server hold four of its client's IDs instead of
  two. This brings back the limit-4 space the two-ID change had dropped; existing inputs keep their meaning.

## Evidence

- New `[net-path]` case "a path whose check ends in its own packet ...": a probe-only answer gives r's ID up after the
  packet, then w's challenge leaves on the next ID; with a challenge and a PING, the server moves to r, answers on r and
  retires nothing. The moving variant fails on HEAD's engine. The probe-only variant also passes on HEAD, so it guards
  the deferred drop.
- Corpus: adopted 1ac9ab1b (the minimised 57-byte crash with 0xF3 inserted, 58 bytes). It fails on HEAD's engine and
  passes with the fix. The 46 committed inputs replay clean.
- Teeth: deferral removed (the new case fails, and so does ddaef1b3); deferred drop never done (the new case fails);
  pre-fix engine (the new case and 1ac9ab1b fail).
- Not caught by any test: dropping the `m_receive_path` reset in `drop_path()`. It only matters for held packets read
  after a deferred drop.
- linux-gcc-debug: 53 CTests pass (`crd-net`, `crd-check*`, `crd-no-*`, `crd-fuzz-net*`) and `[net-path]` 294 cases.
  linux-gcc-asan (with UBSan): `[quic-connection],[net-path]` pass, and every other net case passes too.
- libFuzzer, 280 s each, no artifact: 26403 runs from the committed corpus, and 25032 runs from the 23 over-paths
  inputs with 0xF3. LLVM 22.1.8 tidy is clean on the three touched sources; the added lines need no clang-format change.

## Next

CI first: audit fuzz host-check 45099801433, then dispatch one on this batch. Then NET-PATH's scheduling: an idle path's
RTT kept fresh, and PATH_ACK placement.
