# 2026-11-25: NET-PATH, where crd-net's bulk sender spends against msquic's

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), Remaining item "crd-net's bulk path brought to msquic's cost". Follows
[shared TLS settings](2026-11-25-net-path-shared-tls-settings.md). Ran on this PC (WSL2, linux-gcc-release and -debug).
No engine change landed: this batch measured, and its candidate was refused by the suite (below).

**CI first:** nothing to fix. Every job of CI 45466383581 (6e4b4728) ended green apart from the reds already fixed on
main (the GCC LTO null-dereference in c0dfe5a8, the `min_u32` in 80411130); win-asan and linux-gcc-debug passed.
CI 45496952342 (a3928095, holding 89e8290c and 5ce9ad3e) was still running, no job failed.

## Findings

- Board at a3928095 (1 GiB, 1 MiB windows): 577 MiB per CPU-second (server 1,474, client 948) against msquic's 853
  (1,867 and 1,571), 1.48 times. Per GiB over three runs: crd-net's client 0.83-0.90 s user and 0.20-0.28 s system,
  server 0.45-0.50 and 0.22-0.26; msquic 0.44-0.49 and 0.13-0.16, server 0.41-0.43 and 0.10-0.12. Messages sent
  34.2-34.6 thousand against 18.5-18.8, the server's ACK datagrams 16.3-16.8 thousand against 8.7-8.8. Most of the
  gap is the sender's user time: with no system time at all its client would still cost more than msquic's.
- Trains of 22: a send quantum drains about 46 datagrams (64 KiB and the pacer's credit refilled while they are
  written), and `SendQueue::send_through` hands a socket 32 at a time (`kSendBatch`), which cuts each drain in two.
  With 256 a call the drain goes as 45 and 1 (the 65,507-byte train limit) and messages fall only from 34.5 to 32.8
  thousand; 256 send slots change nothing; a burst capped at 45 datagrams still drains 46 (the refill).
- Profile (perf, DWARF stacks, 1 GiB, 7,747 client samples): sending 82%; `write_packet` 68%, of it the AEAD seal 17.8%,
  `write_streams` 17.3% (its copy of stream data 6.3%), an ACK frame written into every packet 5.7%
  (`QuicAckTracker::write`, `can_acknowledge`), `carried_ack_ns` 5.8%, `has_data` 4.9%, two SipHash draws a packet
  1.5% (packet-number skip and grease bit), the PTO arm 1.6% and the pacing rate's 128-bit division.
- An ACK of the receiver's ACK-only packets in every sender packet (RFC 9000 section 13.2.1 lets it wait). Three
  rules were tried and none landed; each changed the simulator's lossy and multipath cases:
  - owed only, plus one a smoothed RTT for the 13.2.4 retirement: board +6.9% (median 592 against 554 MiB per
    CPU-second); red: client migration seed 3 (validation 1 s instead of 60 ms), the dark shortest path (4 ACK
    carrier moves against 2), LEO CUBIC aware against blind;
  - every packet while a packet carrying more than PING is unreported, else one a round trip: +5% (577 against 549);
    red: Prague's L4S queue (4,512 against 4,000 bytes), a handover schedule lock and a dark-path PTO order;
  - every packet while any ack-eliciting packet is unreported: 4 reds, and the 10 ms case kept an ACK in 647 of 749
    packets (the receiver's PING every 16 ACK-only packets comes sooner than a round trip).
  - Over 20 seeds of the migration case its medians are level (a third at 1,079 ms both, the end 2,003 against
    1,972); seed 1 alone is slower. The patches are kept outside the tree for the next attempt.

## Next

1. The sender's per-packet costs that change no behaviour: the gates evaluated again for each datagram
   (`carried_ack_ns`, `has_data`), one SipHash draw a packet and the pacing division. Proof: every simulator case's
   assertion count equal to HEAD's (LEO: 151,986,309), the board in interleaved rounds.
2. Then the ACK rule again, each red case traced to its cause before it lands.
