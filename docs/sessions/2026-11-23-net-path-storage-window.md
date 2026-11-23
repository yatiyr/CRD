# 2026-11-23: NET-PATH, the storage-full wait classified, the datagram floor case and picoquic multipath

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follow-up of
[the ACK-only storage note](2026-11-23-net-path-ack-only-storage.md): classify 9976136a's wait and add a direct case
for a datagram over a probe timeout's floor.

## What landed

- **9976136a no longer waits.** Replayed under gdb at HEAD, the follow check runs once, unpinged; the server follows
  after the first PING. Its 0.77 s wait was gone with the ACK-only eviction.
- **The `can_send` clause is still needed, and it is designed behaviour.** With the clause removed, libFuzzer failed
  the check after 38,411 runs (input 20207cdb). The client's slot 0 holds 72 of 72 entries, all ack-eliciting and
  outstanding:
  - 68 were sent on the old 4-tuple, below `path_floor` (161), which the peer never acknowledges;
  - 2 are off-path challenges, padded to 1200 bytes;
  - 2 are 37-byte PINGs on the new 4-tuple (74 bytes in flight).
- **Why it waits.** The storage is the configured bound on what a connection holds in flight (`sent_packets`,
  default 128). The timer is the slot's Probe timeout: the PINGs sent at 9.419 s, plus 2.048 s (the reset estimate
  with one backoff), gives 11.467 s. That timeout's `make_room` declares the oldest packets lost. This is the
  documented storage window, not a defect. 20207cdb is adopted into `corpus-quic-connection` as the clause's
  regression input.
- **Direct `[quic-datagram]` case:** a client whose probe timeout keeps it to the 1200-byte floor queues a 1400-byte
  path's largest datagram with a carried ACK pending. It sends nothing for it. The ACK goes alone only at its
  deadline, and the datagram follows the server's acknowledgement.
- **Multipath against picoquic 2026.11.08** (its RFC 10069 codepoints match the register's). New options:
  `--multipath N` and `--add-path-at N` on crd-quic-peer, and `--multipath 1` on crd-picoquic-quic-peer. Both
  report `multipath`, and the picoquic peer also reports `paths`, `largest_path_id` and `other_path_received`. The
  picoquic peer now sums its ECN counts over its per-path spaces. crd-quic-peer's migration names were stale since
  NoMultipath and NoPathId; they are now complete in the enumeration's order, held by a `static_assert`.

## Evidence

- Mutant: the datagram site lets the carried ACK ride alone. The new case fails with a 42-byte ACK-only packet.
  Restored.
- linux-gcc-debug: the `[quic-connection]`, `[quic-datagram]`, `[recovery]`, `[multipath]` and `[net-path]` families
  pass 427 cases (7 skipped). The connection corpus replays 63 inputs.
- clang-format is clean on the added lines.
- `picoquic_interop.py --only multipath`: 12 of 12, in v1 and v2 and both roles. In each, both ends offer it, only
  Cerid does, or only picoquic does. With both, picoquic held 2 paths and read 29,769 and 150,172 bytes on path ID 1.
  Before the ECN sum, the ECT(1) check failed, so the checks read the real counts. Every loopback case passes (30 of
  30), and so do both classic-AQM relay cases.

## Next

- CI first: host-check 45187161054 and the first finished CI run containing 94067d9c, 785c1f64 or this batch.
- picoquic multipath past the first path: a picoquic client opening its own path (a second socket in its peer,
  `picoquic_probe_new_path_ex`), either end abandoning a path, and PATH_STATUS backup both ways.
