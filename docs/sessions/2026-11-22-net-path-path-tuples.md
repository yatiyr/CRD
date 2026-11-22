# 2026-11-22 — NET-PATH: each path slot's own 4-tuples

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), "Paths and
identity". The previous note's first decision, settled before the server-side open.

## CI first

Nothing to fix: no run on this track's commits has finished (CI 44963711021 on d8163867 pending, fuzz host-check
44963728694 queued).

## Decision

RFC 10069 section 3.1.2 makes a packet on a path ID's ID from a new 4-tuple RFC 9000's migration within that path ID,
and section 5.2 lets two path IDs share a 4-tuple. One shared 4-tuple table would make their ID sequences collide
and let one path ID evict another's active 4-tuple. So each path slot keeps its own `QuicPaths` (S75), with its
amplification credit (section 7.2 applies the limit per path), the packet that last chose a 4-tuple and whether the
peer followed a move.

## What landed

- `refactor(net)` 94e3af35: `QuicConnection::Tuples` per slot (the handshake's inline, the others to be allocated
  with their slot); the S75 helpers take the slot and use its path ID's connection IDs; `datagram_slot()` names a
  datagram's slot by its first packet's Destination Connection ID, and a packet of another slot is dropped;
  `write_packet` sends to an ID of its slot's path ID and reads that slot's largest acknowledged. The client's moves,
  the preferred address (section 2.2), path expiry and the path timers stay the handshake slot's.

## Evidence

- `crd-net-tests "[net]"` on linux-gcc-debug (WSL): 867 cases, 860 passed, 7 skipped, as before.
- `ctest -R "crd-check|crd-no-|quic|net-" -E "netns|router"` on linux-gcc-debug: 237 of 237 pass.

## Next

The server-side open with the raised limit, in one slice: every further slot's recovery storage, control, ID space
and 4-tuples allocated at construction; ID spaces opened at negotiation and IDs issued up to the smaller limit; a
path ID's slot opened at its first authenticated packet (a tentative protection slot rolled back when it fails);
PATH_RESPONSE, PATH_CHALLENGE and PATH_ACK in that slot; PATH_ABANDON both ways with three PTOs of retention; each
slot's expiry and timers; `ack_frame` refusing a PATH_ACK of a path ID never opened; the PATH_STATUS table freed as
path IDs close. Proof through `RawRun` opening path ID 1 (its protection's `open_path`), then the fuzz oracle.
