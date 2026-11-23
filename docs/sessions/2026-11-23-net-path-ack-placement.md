# 2026-11-23: NET-PATH, PATH_ACK placement

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S78](../design/network-foundation.md#multipath),
Scheduling and Proof bullets. RFC 10069 sections 3.1, 3.3, 5.4 and 5.5 and RFC 9000 section 13.2.4 read for it.

## What landed

- The path choice places acknowledgements. Under `LowestRtt` every path's PATH_ACKs ride one carrier: the lowest
  smoothed RTT among the paths that can carry them now (`carries_acks()`). `choose_ack_slot()` keeps the carrier
  until another path is an eighth shorter, and counts each move in `ack_path_moves`. Under `Turns` (also switched to
  live), and while the carrier cannot send, each path acknowledges itself (`ack_carrier()`).
- Every reader of an ACK deadline goes through `carried_ack_ns()`: the timer, `has_content()`, the PMTU probe's
  ACK-first rule and the carried-along ACK. So an owed acknowledgement always belongs to a slot that can send it.
- A ledger record (`kRecordPathAck`) settles each rider's PATH_ACK, and its acknowledgement retires the ranges it
  reported (RFC 9000 section 13.2.4).
- Fate sharing. A packet that carries other paths' acknowledgements of more than PINGs (`m_beyond_ping`) gets a PING
  one round trip after the carrier's last ack-eliciting packet, while none is in flight. With one PING outstanding,
  its probe timeout is never pushed back. A carrier whose return goes dark is therefore left within one probe timeout.
- Acknowledgements of PINGs alone never elicit. Without that rule, two ends whose carriers differed traded PINGs once
  a round trip while idle: the refresh case's idle check caught it, at about 50 datagrams in five seconds from each
  end.

## Evidence

- Four new `[multipath]` cases in `test_quic_connection.cpp`:
  - The client's estimate of a path 30 ms slower each way: 70 to 75 ms under the server's `LowestRtt`, 100 to 105 ms
    under `Turns`. The server sends nothing on path ID 1 and its ranges retire. Backup status moves the PATH_ACKs
    once.
  - The carrier dark toward the client or both ways, under a flood, a trickle and pinned datagrams: the PATH_ACKs
    move away within 400 ms (250 ms for the trickles), everything sent arrives meanwhile, and they move back
    once the carrier works again.
  - Two paths of one RTT under 6 ms jitter: at most one move per end. Without the margin, 26 moves.
- Adapted cases: the raw server and the dark-path cases now use `Turns`, because their checks rely on per-path
  acknowledgements. `WindowWatch` now starts at the slot's next packet; before, it counted what was already in
  flight.
- Ten source mutations, each caught: no margin; no round PING; round PINGs piling up; no ack of ack; placement
  regardless of the choice; a backup carrier; a dark carrier; PINGs eliciting; no placement; riders' deadlines not
  owed. The reclaimed-slot case, over its link losing and duplicating 20%, passes seeds 1 to 64.
- linux-gcc-debug: crd-net-tests and the net corpora pass, and so do the 19 `crd-no-|crd-check` guards.

## Next

- S78's next connection slice: the fuzzers opening path IDs (the connection fuzzer's scripts open none yet).
