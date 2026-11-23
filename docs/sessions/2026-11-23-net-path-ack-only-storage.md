# 2026-11-23: NET-PATH, ACK-only packets give up their storage

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Sending bullet of "QUIC recovery" and the frame-ledger
bullet of "The QUIC connection" in [network-foundation.md](../design/network-foundation.md). Follow-up of
[the preferred rebinding note](2026-11-23-net-path-preferred-rebinding.md): revisit the follow check's `heard` clause.

## What landed

- **The follow check, tightened.** With `heard` made unconditional, only f370db2c and 9976136a fail. In both, the client
  could not record a packet. In f370db2c it held 62 ACK-only and 10 eliciting entries. In 9976136a it held 71
  outstanding eliciting entries, but its slot counted 0 bytes in flight and no probe timeout. It sent at its next timer,
  0.77 s later, and the server then followed. A pinged client may now stay unheard only while `can_send` is false.
- **Engine defect 1 (f370db2c).** Behind a dark return path, ACK-only entries filled the storage, so even a PING waited
  for a backed-off probe timeout. Now `on_sent` gives the oldest outstanding ACK-only entry to the next packet, keeps
  the order, and reports the discard as an event. `can_send` and `can_send_data` count those entries as room, through
  a maintained counter.
- **Skipped runs per entry.** Each entry records the run skipped just before it (`unsent_from`). `AckOfUnsent` reads
  those runs instead of holes, so a given-up number reads as sent. A run also stays unsent once older packets settle,
  which the old hole rule missed.
- **Engine defects 2 and 3, uncovered by the change.** Storage had been capping two ACK-only storms:
  - A waiting datagram over a probe timeout's 1200-byte floor: the server sent 500 packets in one flush. Now an ACK
    that is not owed yet rides only with other frames.
  - A retirement owed while the frame ledger was full: a client sent 46,962 datagrams. `has_data` now counts
    record-settled frames, CRYPTO included, only while the ledger has room.
- **Fuzz.** In the connection fuzzer, each end must now drain within the 64-datagram flush guard. Recovery fuzzer
  model: a send discards at most the oldest spare ACK-only packet. The storm input 371211f7 is adopted.

## Evidence

- Mutants, each caught and then restored:
  - No eviction: the new connection case fails (storage full), and so does the recovery case.
  - Retirement not gated: 371211f7 fails `drained`.
  - No range retirement: the retitled ACK-run case fails.
  - No ACK-run PING: case 2533 fails.
- The new skipped-run checks fail under the old hole rule: an ACK of 1 after 0 settles, and an ACK of 0 to 2.
- linux-gcc-debug: crd-net-tests passes 1153 cases (7 skipped); every `crd-fuzz-net`, `crd-no-` and `crd-check` test
  passes. linux-gcc-release with LTO: the touched suites, both corpora and the fuzz replays pass.
- libFuzzer, connection fuzzer: runs of 374 s and 487 s found nothing. A 500 s run before the ledger fix found only
  the slow unit, now fixed. Recovery fuzzer: 241 s and 301 s, no artifact.
- clang-format is clean on the added lines, and the brace check passes. The win-tidy build of crd-net-tests (every
  touched engine source and test) has no warning.

## Next

- CI first: host-check 45187161054 (5871744e) and the first finished CI run with this batch. A `drained` failure is
  either a storm or a legitimate 64-datagram burst; classify it first.
- 9976136a: is the 0.77 s timer the probe timeout of packets from before the path floor (storage as window), or
  a fourth defect? Check the client's `pto_count` across it, the timeout kind and the entries against `path_floor`.
- A direct `[quic-datagram]` case for a datagram over a probe timeout's floor. Today only the hostile datagram
  case's flight bound catches it.
- Then picoquic interop for NET-PATH.
