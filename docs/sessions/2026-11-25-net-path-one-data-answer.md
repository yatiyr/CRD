# 2026-11-25: NET-PATH, one answer a datagram to whether data waits

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), Remaining item "crd-net's bulk path brought to msquic's cost". Follows
[the learner's keep](2026-11-25-net-path-learner-keep-background.md), Next 2. User decision on record (2026-11-01): "I
WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" Ran on this PC (WSL2 linux-gcc-release with LTO,
-debug, linux-clang-fuzz; MSVC win-debug).

**CI first:** nothing to fix. 45601823342 (1dbff857) had no failed job, 15 still running; 45617899706 (192f6205)
pending.

## Landed (no change in behaviour)

- `gate_data()` returns the `has_data()` answer it found for the datagram it opens, and `has_content()` takes it for
  the Application space: one search a datagram instead of three. Nothing between them (the PMTU probe's zero paths)
  changes what `has_data()` reads.
- `QuicPathMtu::raise_due()`: `probe()` reads `busy` only once the raise timer has passed, so the connection asks for
  it only then. The ACK-before-streams check in `write_frames()` asks the stream set only when an ACK waits.
- Profile (perf, DWARF, the 1 GiB client): `QuicStreamSet::has_frame` from 4.56% to 1.71%; `has_data` off the list.

## Evidence

- Debug suite, each of 1,208 cases in its own process (seed 1), HEAD against the change: equal assertion counts but
  the four that vary run to run on HEAD (QMux stalls, 0-RTT first service, 0-RTT before a Retry, the ticket cache)
  and the two PMTU cases given `raise_due` checks (+3 each). MSVC win-debug: `[quic-pmtu]` 29 cases (91,382,845
  assertions) and `[quic-connection]` 195 cases (1,244,776,976) pass.
- Fuzz (`-libfuzzer` drivers, scratch corpora): the PMTU fuzzer now asserts `probe()` gives the same size and state
  for either `busy` when the raise is not due; 2,105,455 and 2,219,533 runs clean (seeds 2 and 1), congestion 349,134
  and connection 7,168 runs clean. Two earlier seed-1 runs stopped on impossible faults (a UBSan load on the stack
  World, a write to 0x4 in `start()`'s `*this =`); both inputs replay clean and a seed-1 rerun is clean: this host's misexecution.
- Teeth: `gate_data` reporting data always waiting turned 142 cases red (ECN, PMTU, BBR, GCC, Prague, migration);
  `raise_due` held false turned 4 PMTU cases red. Restored; the rebuilt binary is byte-identical to the measured one.
- Board, interleaved against HEAD's build, best (median): 1 MiB windows 693 (686) against 654 (654) MiB per
  CPU-second, +6.0%; msquic 979, now 1.41 times. 64 KiB 495 (490) against 483 (470), wall 738 against 748 MiB/s;
  handshakes 4,593 (4,590) against 4,624 (4,612), level. Datagrams and ACKs per MiB equal.

## Next

1. CI first: the first finished run containing 02ea3dff, 2cff089a, ca841b60 and this change (MSVC, clang-cl, ARM64,
   TSan), and win-asan's TP-17 gate lines.
2. The ACK rule (an ACK frame in every packet), each red of the three earlier attempts traced to its cause first.
