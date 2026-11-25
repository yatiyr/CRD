# 2026-11-25: NET-PATH, an ACK of ACK-only packets once a round trip

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), Remaining item "crd-net's bulk path brought to msquic's cost" (its first
named cost: an ACK frame in every packet). Follows [the single data answer](2026-11-25-net-path-one-data-answer.md),
Next 2. User decision on record (2026-11-01): "I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" Ran
on this PC (WSL2 linux-gcc-debug, -release with LTO, linux-clang-fuzz; MSVC win-debug).

**CI first:** nothing to fix. 45601823342 (1dbff857) had no failed job; win-asan shard 1 printed
`registered TP-17: passed`. Later push runs were cancelled or pending.

## Landed

- **The ACK rule** (RFC 9000 section 13.2.1 lets an acknowledgement of non-eliciting packets wait). A 1-RTT packet
  carries its path's ACK while a received packet carrying more than PING is unreported by any acknowledged ACK frame
  (`QuicAckTracker::awaited_unreported`, cleared when an acknowledged frame reaches the largest ack-eliciting number),
  or one is owed; otherwise one packet a smoothed RTT (`ack_ride_ns`), so a bulk sender's ACK-only receiver still has
  its ranges retired each round trip (13.2.4). `acks_sent` counts ACK and PATH_ACK frames.
- **The LEO measurement** (`leo_transfer`): a handover counted as measured when its span opened, but its bytes only
  when the span closed, and SCReAM/GCC compared byte sums over different counts. With the rule, GCC's blind run ended
  at 20.2 s instead of HEAD's 19.3 s (three HEAD runs pass), so its second span closed and two spans' bytes were
  compared against the aware run's one. A handover now counts when its span closes, and the comparisons are per
  measured handover (`reads_more_after`); the 2 s case's local `kRuns` is `runs` (clang-tidy's local-constant case).

## The earlier attempt's reds

They no longer reproduce, each case in its own process with the rule: the handover lock (ca841b60 fixed the keep),
the dark path's PTO order and Prague's L4S queue (both pass after the canary, gate and draw changes since). The only
red was the GCC row above, a measurement artefact the rule's timing shift exposed.

## Evidence

- Debug suite, every case in its own process (seed 1): 1,202 pass, 8 skipped (Windows RIO), with the rule and the
  fix; `[handover]` three times on each of HEAD+fix and the rule: all pass but two HEAD+fix runs that died part way
  (both pass serially). MSVC win-debug: `[recovery]` 48 cases,
  `[quic-connection]` 196 cases (1,245,820,432 assertions) and the GCC row pass.
- New cases: the tracker's awaited flag (PINGs, ACK-only packets, an acknowledged frame short of the largest, copies,
  reset) and a 1 MiB transfer whose client writes 740 datagrams and fewer than one ACK frame in sixteen.
- Teeth: an ACK in every packet turns the new transfer case red (740 ACK frames of 740 datagrams); never riding
  unless owed turns "a server that only acknowledges retires what its ACK frames reported" and "a quiet client's
  move reaches its server" red; a retirement one short trips the fuzzer's new oracle within 150 s. Restored; the
  rebuilt test binary is byte-identical to the measured one.
- Fuzz: the recovery fuzzer models the awaited flag (bit 3 of a received packet's second byte marks a PING alone):
  334,329 runs clean (seed 2); connection 12,747 clean. A seed-1 recovery run stopped on a UBSan pointer overflow in
  `space_of` that replays clean four times. Five debug runs of HEAD's binaries and one of the rule's died part way
  (two SIGSEGVs in one-process suite runs, a stack-canary failure inside `operator""_catch_sr` under gdb, three
  partial cases), each passing serially, while the hosted ASan lanes on 1dbff857 passed: this host's misexecution.
- Board (1 GiB, five interleaved rounds, best (median)): 1 MiB windows 724 (716) against HEAD's 679 (668) MiB per
  CPU-second, +6.6%; msquic 981, now 1.36 times. 64 KiB 503 (490) against 492 (491), wall 794 against 773 MiB/s.
  Data datagrams per MiB 735 against 739. The congestion simulator grid drives controllers alone, so no row moves.

## Next

1. CI first: the first finished run containing this change (MSVC, clang-cl, ARM64, TSan; `on_received` gained a
   defaulted parameter in two classes).
2. GCC's blind LEO run over seeds 0 to 3, HEAD against the rule: its elapsed time (19.3 s against 20.2 s at seed 3).
3. NET-PATH's bulk cost: the trains a flush batch cuts (`kSendBatch` 32 against a 46-datagram quantum).
