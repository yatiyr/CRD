# 2026-11-21 — NET-PATH: packet protection per multipath path

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath), the new
"Protection per path" bullet. Source: RFC 10069 sections 2.3 to 2.5 and 3.1.

**CI first.** Fuzz host-check 44934840697 (7409d1b8) is still queued, so there was nothing to audit.

## What landed

- `QuicPacketProtection` keeps one 1-RTT number space per path slot (`kQuicMaxPaths`, which moved to quic_frame.hpp
  so quic_packet.hpp needs no path header). Each slot holds its path ID, its replay window, and the packet numbers
  the key phases are checked against. `open_path`/`close_path`/`path_open`/`path_id` manage the slots. `seal`,
  `open`, `largest_opened` and `acknowledged` take the slot (0 by default), and `QuicPacketWriter::seal` takes the
  path ID for the nonce. The connection still passes slot 0: path IDs come from connection IDs in the next slice.
- The key phases stay the connection's, but every number comparison stays within one path's space. A path that has
  opened nothing under the current key sends a mismatched phase bit to the previous key while it is held (section
  2.5). The global version would have closed the connection with KEY_UPDATE_ERROR on a new path's first packets
  after an update, and tried a late packet on a quiet path under the next key, where it fails as forged.
- An acknowledgement allows the next update only if it covers the first packet the current phase sealed on that
  path. A late previous-phase packet now also sets the path's older bound (its flag was left unset before).

## Evidence

- linux-gcc-debug: `crd-net-tests "[quic-packet]"` 13 cases and 1817 assertions; `"[net]"` 857 cases (850 passed,
  7 skipped as before).
- The new `[multipath]` case covers slot refusals and close; one number on every path; copies and stale numbers per
  window; a one-byte number decoded against its own path (forged as path 0's 45); forged opens across paths under
  one integrity limit; a reused slot; key phases across three paths with per-path 6.4 bounds; and acknowledgements
  per space.
- Packet fuzzer: control bit 3 runs a round trip on slot 1 to 3 under path ID 1 to 15 or 2^32 - 1, across key
  updates. Its oracle requires the packet to be forged in slot 0, to open in its own slot, and to be that slot's
  largest. Two new seeds; the eight committed seeds regenerate byte for byte; the 18-input replay is clean.
- libFuzzer (linux-clang-fuzz), two sessions of 2 jobs for 240 s, about 1.75 M runs at 1831 edges. Each session
  stopped one job on an input that does not replay, three times each, on the libFuzzer binary or linux-gcc-debug.
  The second stop was a wild 136 TB memcpy inside AWS-LC from a stack-garbage return address, in a received-mode V2
  input that never reaches the path code. That matches the host's known pattern (`host-misexecutes-local-faults`).
  The 620-input working corpus replays clean. CTest guards (`crd-no-*`, `crd-check*`, the packet corpus): 20 of 20.
- clang-tidy was not run here (the win-tidy tree's PCH does not load in standalone clang-tidy); CI's win-tidy runs.

## Next

Connection IDs per path ID (S78's next bullet): issue, rotate and retire within each path ID, and have the host's
connection table name the path ID (and so the protection slot) of each datagram's Destination Connection ID.
