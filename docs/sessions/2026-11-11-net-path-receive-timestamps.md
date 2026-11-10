# 2026-11-11 — NET-PATH: QUIC receive timestamps feeding GCC

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S73](../design/network-foundation.md#receive-timestamps).

## CI first

Nothing of this track's to fix: host check 42906546427 (the GCC fuzz legs) and the TSan, ASan and debug jobs of push
run 42856002748 were still running; every newer push run was cancelled before it started.

## What landed

- draft-ietf-quic-receive-ts-03 read in full; `quic.receive-timestamps` is implemented in the standards register
  with its four temporary codepoints (two transport parameters, ACK_RECEIVE_TIMESTAMPS with and without ECN).
- Transport parameters: both read and written; the exponent is bounded at 20 and ignored without a maximum. Both
  draft identifiers have GREASE's 31 * N + 27 form, so the writer's GREASE parameter now steps past identifiers the
  module reads.
- Frames: ACK_RECEIVE_TIMESTAMPS read (ranges above the largest or below packet 0 and times below the basis are
  FRAME_ENCODING_ERROR) and written from the reporter's times, `QuicAckTimestamps` walking them.
- Recovery: `QuicReceiveTimestampLog` reports the latest arrivals of packets the frame acknowledges, in the room the
  ranges leave; `QuicPeerReceiveTimes` reads a report for the packets it newly acknowledges, with the draft's
  plausibility check, and the connection hands each time to the controller as `received_ns`.
- Connection: `parameters.max_receive_timestamps_per_ack` (at most 32) is the flag. A frame never asked for is
  FRAME_ENCODING_ERROR; outside 1-RTT or past the maximum, PROTOCOL_VIOLATION. Counter `receive_timestamps`.

## Evidence

- win-debug (MSVC): `crd-net-tests` 932 cases pass; the new cases are tagged `[receive-timestamps]`. The codec,
  recovery and connection corpora, with new seeds, replay clean. WSL linux-gcc-debug and linux-gcc-release (LTO):
  145 QUIC frame, parameter, recovery, connection and congestion cases pass, and the three corpora replay clean.
  clang-tidy 20 is clean on the 17 changed sources; its four findings (nested conditionals, a loop for `any_of`)
  are fixed.
- GCC through QUIC, 2 Mbit/s, 50 ms, a 300 ms buffer: on receive timestamps it delivers 93.0% of the link with at
  most 32 ms queued and 12218 packets timed. From ACK frames alone it delivers 93.3% with 35 ms queued; CUBIC queues
  291 ms. Reports flow only when both ends ask for them, and they hold over lossy, duplicating and reordering paths
  (4 seeds).
- Interop: no pinned peer implements this draft (their trees were searched; picoquic's 0x7158 is
  draft-huitema-quic-ts).

## User decisions recorded

2026-11-01: "approve the downloads we need to do everything properly, I will contect a lawyer later! DO NOT BLOCK
ANYTHING CONTACTING A LAWYER IS ... HARD! I WANT NETWORK PART TO BE COMPLETE AND PROPERLY DONE AND GOLD STANDARD AND
ELITE".

## Next

GCC's probing (libwebrtc's start and ALR probes), then migration, NAT rebinding and LEO handover.
