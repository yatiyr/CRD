# 2026-11-23: NET-PATH, the storage-full wait classified, the datagram floor case and picoquic multipath

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Follow-up of
[the ACK-only storage note](2026-11-23-net-path-ack-only-storage.md): classify 9976136a's wait and add a direct case
for a datagram over a probe timeout's floor.

## What landed

- **9976136a no longer waits.** Replayed under gdb at HEAD, the follow check runs once, unpinged; the server follows
  after the first PING. Its 0.77 s wait was gone with the ACK-only eviction.
- **The `can_send` clause is still needed, as designed behaviour.** Without it libFuzzer failed the check (input
  20207cdb, now in the corpus). The client's slot 0 held 72 of 72 ack-eliciting entries: 68 from the old 4-tuple
  below `path_floor`, 2 off-path challenges and 2 PINGs on the new one. The storage is the configured in-flight
  bound (`sent_packets`, default 128); the slot's probe timeout (9.419 s + 2.048 s) makes room. Not a defect.
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
  The check asks only for some bytes there, which a validation alone would meet. Every loopback case passes (30 of
  30), and so do both classic-AQM relay cases. At HEAD the libFuzzer target replays the corpus, 19 guards pass and
  the win-tidy build of crd-net-tests has no warning.

## Next

- CI first: host-check 45187161054 and the first finished CI run containing 94067d9c, 785c1f64 or this batch.
- A stream pinned to path ID 1 in the picoquic cases, so picoquic must read it there. Then a picoquic client's own
  path (`picoquic_probe_new_path_ex`), abandoning a path, and PATH_STATUS both ways.
