# 2026-11-25: NET-PATH, a send quantum of one host train, and flush calls that never cut one

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path), Remaining item "crd-net's bulk path brought to msquic's cost". Follows
[the once-a-round ACK](2026-11-25-net-path-ack-once-a-round.md), Next 2 and 3. User decision on record (2026-11-01):
"I WANT NET DONE PROPERLY AND GOLD STANDARD AND WITHOUT ANY DEFERS!" Ran on this PC (WSL2 linux-gcc-release with LTO,
-debug, linux-clang-fuzz; MSVC win-debug).

**CI first:** nothing to fix. 45601823342 (1dbff857) has 30 jobs passed and 2 running (TSan, win-relwithdebinfo);
45638204334 (cc2c0851, the first with the ACK rule) is pending.

**Next 2, GCC's blind LEO run:** scatter, closed. Over the fixture's seeds (they repeat modulo 4), the spike case's
blind GCC with the ACK rule against it reverted: 19.62/19.78, 15.39/15.42, 20.00/19.86, 20.20/19.31 s; the 25 ms
step's blind run 23.34/23.90, 24.38/24.07, 24.22/23.92, 23.75/23.40 s (+0.4% summed); every aware run is faster or
level (spike 3 of 4, step 4 of 4). Seed 3's thinnest committed margin is SCReAM's bytes after a handover, 2.2%
(deterministic); GCC's is 10%.

## Landed

- **The cause** (a temporary histogram of each `sendmmsg` message, not kept): every 1 MiB-window quantum was 45
  datagrams of 1472 bytes, 64 KiB of pacer credit, and an IPv4 train holds 44 (65,507 bytes), so each left as a
  train of 44 and one alone: 22.5 a message. Raising `kSendBatch` alone moved nothing (23.0 against 22.2).
- **`quantum_max`** (congestion.cpp): a window controller's train cap and burst and BBR's send quantum are at most the
  whole datagrams one host train carries (44 of 1472 bytes, 54 of 1200), held on a datagram resize too.
- **`SendQueue::send_through`**: up to 128 datagrams a call (two longest trains), and one datagram past the batch
  shows whether its last train goes on: a segmenting socket's call ends before a train it would cut.

## Evidence

- Board (1 GiB, five interleaved rounds against cc2c0851's build, best (median)): 1 MiB windows 744 (732) against
  712 (706) MiB per CPU-second, +4.5%, wall 1,232 against 1,167; 43.3 datagrams a send message against 22.3 (msquic
  43.4), msquic now 1.32 times. 64 KiB: 499 (496) against 499 (494), wall 781 against 777 (the flight-burst rule keeps
  its trains at 14.5).
- Simulator grid (NewReno, CUBIC, BBR; 1 Gbit/s and 100 Mbit/s; exact, 250 us and 1 ms turns; 12 KB to 256 KiB; 2 and
  20 ms; 10 s): 120 of 144 rows identical. Window controllers at 1 Gbit/s, 1 ms turns: 64 KiB queues drop 0 where
  they dropped 10 and 6; 256 KiB queues 51.7% against 52.7% and 50.5% against 51.4%, the 55th datagram a turn the
  simulator scored free and the host sends alone (54/55 = 0.982 = 51.7/52.7); version 7 holds 9.6% there. BBR 64 KiB at
  1 ms turns: 51.3% with 0 drops against 45.3% with 500. BBR exact, 32 KiB, 2 ms: 93.7%/1,377 drops against
  92.8%/812, the burst alone (a variant with BBR's old quantization budget equals it). Around it (queues 28,000 to
  40,000 bytes, one-way 0.9 to 1.25 ms, 24 rows) the mean link is 93.82% against 93.78% and drops +1.7%, all from that
  row; the same at 250 us turns 59.24% against 58.40%, drops +3.3%, rows scattering both ways; the 250 us, 32 KiB, 2 ms
  row over eight seeds spans 58.6 to 63.9% against 59.3 to 61.5%.
- Tests: the cap and burst at five datagram sizes, a resize, a full pacer releasing 44 not 45; a call boundary keeping
  trains whole (4 trains in 2 calls where 32-datagram calls made 7 in 6), with a busy host and a refusal inside a cut
  batch keeping order. Teeth: a 64 KiB `quantum_max` fails 12
  assertions (45 released); a batch that ignores trains makes 5. Debug crd-net-tests 1,213 cases pass (8 RIO skipped),
  MSVC `[io-loop],[congestion],[recovery],[handover],[udp]` and the Registered I/O cases pass, guards 19 (Linux) and
  20 (MSVC). The congestion fuzzer's oracle bounds the cap by the train: 214,025 runs clean; connection 6,795 clean.

## Next

1. CI first: the first finished run with this change (MSVC, clang-cl, ARM64, TSan; `send_through` and the fixture).
2. A host waking once a millisecond sends one train a turn, about 52% of 1 Gbit/s: more than one train a turn.
3. NET-PATH's bulk cost: the sender's user time per packet at 1 MiB windows (1.32 times msquic).
