# 2026-11-23: NET-PATH, a challenge on path ID 0 answered by a packet that is not probing

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). CI first: fuzz host-check 45165082942 (2e31a3d0), leg
net-quic-connection seed 8402, failed the follow check's `heard` clause on input `ebb520e9`, which also failed on HEAD
(c4fae3b7). Design: [S75](../design/network-foundation.md#migration)'s Validation bullet.

## The defect

A stale non-probing packet of the client's (an older epoch's, from its socket 0's address) was the highest-numbered
one the server had, so the server moved path ID 0 there and challenged the client's real address (RFC 9000 section
9.3.3). The client answered from its active path with PATH_RESPONSE and padding only: a probing packet, which moves
nothing back. Two causes in `quic_connection.cpp`:

- Path ID 1 was the shorter, so the client's path ID 0 carried neither data nor ACKs (they rode path ID 1), and its
  packet took the closed-window branch of `write_frames()`, which never added the PING.
- Where the PING was added, it went only when the packet was not ack-eliciting; a PATH_RESPONSE is eliciting but
  probing. The owed PING could also be taken by whichever slot sent next, though only path ID 0's packet steers
  path ID 0.

## The fix

- `m_answer_owed` (apart from the public `ping()`'s `m_ping_owed`) is set by a challenge on path ID 0's active path
  and is spent only by that slot's packets.
- `Written::steers` records a frame that is not probing (section 9.1): an ACK or PATH_ACK, data, a DATAGRAM or any
  frame `quic_frame_probing()` refuses. `put_answer()` adds a PING to an eliciting packet that does not steer, in the
  closed-window branch too, and alone only where data may go; the answer stays owed until a packet steers.

## Evidence

- `ebb520e9` replays clean on the fixed fuzzer and joins `corpus-quic-connection`.
- New case "a challenge on path ID 0's active path is answered there by a packet that is not probing ..." (two
  variants: path ID 1 carries the stream on, or path ID 0 keeps a pinned stream with nothing to send): an attacker's
  copy moves the server, which is back within 200 ms with no validation failure.
- Mutants: without the closed-window `put_answer(false)` both variants fail (the server stays at the attacker's
  address); without the open-window `put_answer(true)` 11 [net-path] cases fail (the answer is never spent). Letting
  any slot spend the answer fails nothing: the slots' turns give path ID 0 the first send after the challenge in
  these runs, so no case yet separates that restriction.
- The 9-minute libFuzzer run from the corpus then found `d57f5de5`: a pinged client, its slot 0 storage down to the
  entries kept for ACK-only packets and probes (`can_send` true, `can_send_data` false), held its PING, as designed
  (the PING is no probe; its probe timeout makes room, as for 20207cdb). The follow check's escape now reads
  `can_send_data`, what a PING needs; with it the input reaches the follow. It replays clean on c4fae3b7, whose
  extra PING from the old challenge changed the run. Both inputs join `corpus-quic-connection`.
