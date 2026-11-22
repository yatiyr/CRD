# 2026-11-22: NET-PATH, a path that goes dark holds no data back

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: S78's Scheduling bullet in
[network-foundation.md](../design/network-foundation.md#multipath).

## What landed

- RFC 10069 section 5.7: no packet of a path is declared lost before an acknowledgement on it, so a dark path's stream
  data held its stream, its datagrams their outcomes and its frame records the ledger's head until the idle timeout.
  A probe timeout on a slot while another is live (`fresher_path(slot, 1)`) now settles every frame record of the
  slot's packets in flight as lost (`release_frames`, counted in `pto_resends`): the frames go again on the live paths
  within their windows, the packets stay in flight for recovery and the controller, and a late acknowledgement finds
  no record.
- `settle` is split into `settle_frame` and `free_settled_frames`, which `release_frames` reuses.
- A slot with a timeout pending carries no data while another has fewer pending (`held_dark`, ranked): its probes
  are PINGs, and a path dark for long stays held while the live path takes a timeout of its own.
- Defect fixed: `gate_data` opened a slot's window and pacer for any slot's owed probe; it now reads the sending
  slot's own probes only (the handshake spaces on the handshake slot).
- `QuicConnection::path_control(slot)`: each slot's sending state, null for a slot that sends nothing.

## Evidence

- Four `[multipath]` cases in `test_quic_connection.cpp`, with a 64-record ledger, datagrams with outcomes and a
  shared drop-tail bottleneck:
  - Either end sends, and path ID 1 or path ID 0 goes dark both ways. The transfer and every datagram outcome finish
    within 8 s. Later probes add at most 128 bytes in flight each. A send-time watch checks every data datagram of the
    live slot against its window. The path returns and carries data again, with nothing more resent.
  - Path ID 1 dark past four timeouts, then both dark until it times out again, then path ID 0 back alone: path ID
    1's datagrams stay probe-sized throughout.
  - A one-way blackout: the late PATH_ACKs of settled packets count nothing twice, and the stream reaches DataRecvd.
  - Both paths dark: frames move once, to the path not yet known dark, never back; the transfer ends on return.
- Teeth, each alone and rebuilt, each fails its cases:
  - no release (every case);
  - no dark gate (the transfer stalls);
  - records kept after the release (a datagram reported twice, then a stall);
  - every slot's probes opening the window (the watch);
  - an unranked hold, live or not (the long-dark case).
- linux-gcc-debug (WSL): `crd-net-tests` 1125 cases, 1118 passed, 7 skipped; the 19 `crd-check|crd-no-` CTests pass.
- linux-gcc-release (LTO): `crd-net-tests` and `crd-fuzz-net-quic-connection` link clean; `[multipath]` (70 cases) pass.
- linux-gcc-asan: `[multipath]` (70 cases) pass.
- linux-clang-fuzz `crd-fuzz-net-quic-connection`: the 42-input corpus replays clean, then 241 s with 2 jobs, 49261
  executions, corpus 42 to 2096 in a scratch directory, no artifact.
- Pinned clang-tidy 22.1.8 clean on the touched sources (win-debug reconfigured: its database predated the time
  module's include); clang-format applied on the added lines.

## CI

Nothing new to fix. CI 45044166180's win-tidy findings (`kBases`, `kPinned`) were fixed at HEAD by f3e88cde; its
linux-gcc-debug red was the Ninja 403 that fa1c03c7 answers. Fuzz host-check 45051033311 is still queued.

## Next

NET-PATH: the scheduling policy (lowest RTT first, backup paths held back, redundant sends, pinned flows, PATH_ACK
placement), then the fuzzers opening path IDs.
