# 2026-11-21 — NET-PATH: each path's congestion, ECN, PMTU and handover state (RFC 10069 section 5)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath) (Congestion,
ECN and PMTU per path).

**CI first.** Nothing to fix. No run containing this track's last commits has finished. Fuzz host-check 44926199681
(c2d3610f) and the earlier two are still queued.

## What landed

- `QuicPathControl` (quic_path_control.hpp) holds one path slot's sending state: the congestion controller with its
  delivery sampler and pacer, ECN validation, the PMTU search, and the link's handover schedule with its learner. It
  is never copied or moved. Each event's rules moved with it unchanged: a lost PMTU probe and a handover's loss are no
  congestion signal, a spurious loss's data counts as delivered, and the datagram size follows the search.
- The connection keeps slot 0's control inline and indexes all slots through one array. Each recovery event reaches
  its packet's slot after `on_path()`, so a closed slot's packets touch nothing. ACK ends, timeouts and Packet Too Big
  reach the acknowledged, timed-out or quoted slot. Sends are stamped and counted on `send_slot()`, which is the
  handshake's path until scheduling lands.
- `set_congestion` reaches every open slot. The peer's ceiling is each slot's and survives a move. The datagram frames
  take the smallest open path's size, and the pacer timer is the earliest of all slots'. A new port alone `bind`s the
  state; another address `restart`s every part and ends the schedule (RFC 9000 section 9.4). The entropy stream is
  drawn exactly as before.

## Evidence

- win-debug crd-net-tests: 1060 cases, 7 skipped as before (1051 plus 9 new `[path_control]` cases). linux-gcc-debug:
  1061 cases, 7 skipped. linux-gcc-release (LTO) `[path_control],[recovery],[multipath]` and `QUIC paths*`: 71 cases,
  no warning.
- Teeth: dropping the handover loss's early return fails the handover case. Restarting on a port change fails the NAT
  rebinding case in test_quic_connection.cpp.
- Recovery fuzzer: each slot drives its own control, as the connection does. The isolation oracle now also covers each
  control's window, delivery, losses, ECN state, datagram size and pacer timer. Routing `on_lost` to slot 0's control
  fails it on the committed corpus. Its 8 corpus inputs replay clean on win-debug and linux-gcc-debug.
- libFuzzer (linux-clang-fuzz), two sessions of 4 jobs (300 s and 240 s): about 1.6 M runs, coverage 1832 edges (1208
  before). Four jobs ran the full time clean. Four stopped on an ASan SEGV or a UBSan `span` fault. None of the three
  saved inputs replays, three times each, on the libFuzzer binary, linux-gcc-debug or the clang replay build. The
  4161-input working corpus replays clean three times in one process. This is the host's known pattern
  (`host-misexecutes-local-faults`), as last batch; the hosted fuzz host-check on this head decides it.
- 35 win-debug targets that link crd-net were rebuilt: 32 net and wire fuzzers, ceridc, its tests and crd-net-tests.
  The 32 corpora replay clean, and the 20 `crd-no-*` and `crd-check*` guards pass. LLVM-22 tidy is clean on the 6
  changed C++ files. MSVC C4324 is kept off the control and the fuzzer's world, as for the connection.

## Next

Path IDs with their connection IDs and the path lifecycle (S78), so a connection offers `initial_max_path_id` and
opens slots past 0 and lends each one its recovery and control. Then scheduling, coupled congestion control (LIA),
hostile-path and fuzz proof, and interop with picoquic.
