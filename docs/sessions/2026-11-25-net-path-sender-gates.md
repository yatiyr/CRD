# 2026-11-25: NET-PATH, the sender's ACK carrier and send quantum without repeated work

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), Remaining item "crd-net's bulk path brought to msquic's cost". Follows
[the bulk sender's costs](2026-11-25-net-path-bulk-sender-costs.md), Next item 1. Ran on this PC (WSL2,
linux-gcc-release with link-time optimisation and linux-gcc-debug).

**CI first:** nothing to fix. CI 45496952342 (a3928095) had 11 jobs passed and none failed, 11 still running;
45508099418 (80f9dc78) had not started.

## Landed (no change in behaviour)

- `ack_rider()`: the slot carrying other slots' acknowledgements is one answer for every slot, so `carried_ack_ns`,
  the ACK frames' walk and `next_timer_ns` ask it once instead of once per slot. On one path, the old code ran
  `carries_acks` four times per call: `refreshable`, `backup_held` and `can_acknowledge` each time. Each walk also
  tests `path_open` first. A packet's ACK hold now reads `datagram_ready` first and `carried_ack_ns` only after it.
- The send quantum (CUBIC and NewReno `burst`, BBR's `set_send_quantum`) divides by the constant 1,000. It was a
  128-bit `multiply_divide(rate, 1 ms, 1 s)`, which gives the same answer for every u64 rate.
- Profile (perf, DWARF stacks, the 1 GiB client): `carried_ack_ns` went from 5.42% to 0.40%, `__udivti3` to 0.07%.

## Evidence

- Debug suite, every case in its own process (seed 1), HEAD run twice against the change: 1,198 of 1,202 cases
  have equal assertion counts, the LEO case's 151,986,309 among them. The other four also vary from run to run on
  HEAD: QMux seeded stalls, the 0-RTT first service and 0-RTT before a Retry differ between HEAD's two runs, and the
  ticket cache gave 519,402 to 519,419 over five runs of each build. The whole suite passes: 903,454,817
  assertions, 8 skips (Registered I/O, Windows only).
  The release crd-net-tests LTO link is clean.
- The board, interleaved against HEAD's build on a busy host, five rounds, best (median):
  - 1 MiB windows: 513 (503) against 490 (480) MiB per CPU-second, +4.7%; msquic 733 (666), now 1.43 times.
    The data and ACK datagrams per MiB are equal.
  - 64 KiB windows: 440 (435) against 411 (406), +7.1%; wall rate 723 against 655 MiB/s.
  - Handshakes (three rounds): 4,477 (4,449) against 4,537 (4,331), level.

## Refused this batch

One entropy draw a packet, with bit 0 for the grease and six bits above it for the skip, cut a SipHash call. It
changed the random stream, and the LEO case failed for CUBIC: aware made 3 reductions after handovers against
blind's 2, with 1 undone, and was 10 ms slower. This is the same aware-against-blind comparison the ACK rule broke
last batch. The patch is kept outside the tree, in the WSL home's experiment patches (`one-draw.patch`).

## Next

1. Trace why the LEO case's aware CUBIC run undoes a reduction under another skip pattern, fix the cause, then land
   the single draw.
2. `has_data`'s gate for each datagram (2.6%) and `write_streams` (5.9%), then the ACK rule.
