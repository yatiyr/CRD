# 2026-11-10 — NET-PATH: DPLPMTUD against real peers, and the advertised receive slot

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S68](../design/network-foundation.md#pmtu).

CI first: run 42744641737 (45dbd882) fails only `crd-repository-tools`, `crd-generated-sources` and
`crd-check-registers` (`special_refs.inc`'s manifest, the `cerid.game`/`quic.*` entries), all clean at HEAD, and
win-tidy only on `test_direct_search.cpp` (hesap-opt, the other owner's). Nothing of this track is open. No run holding
71b6a04a has finished (42772913465 is pending), so its public-checks read stays owed.

## What landed

- `QuicServerConfig`/`QuicClientConfig::receive_bytes` (the loop's `slot_bytes`, 1500 by default; the server's
  `early_bytes` folds into it): `QuicHost::build` lowers each connection's advertised `max_udp_payload_size` to it, and
  a value outside 1200 to 65527 leaves the host not ready. Cerid advertised 65527 while its loop read 1500.
- `QuicRecovery::on_timeout`: a probe (or anti-deadlock) timeout over a sent-packet storage full of packets nothing
  acknowledges declares the oldest lost so its probes can be recorded and leave (RFC 9002 section 6.2.4). Before, a
  server whose ACKs rode in full packets a fallen path dropped wedged until the idle timeout: 4 of 8 Cerid-to-Cerid
  WebTransport runs. `fuzz_quic_recovery.cpp` holds it to an oracle (only the oldest outstanding, in order, only when
  the storage was full; the space can then send) and a new `full-storage-probe` seed.
- `interop_common.Relay`: `mtu` each way (longer datagrams dropped and counted), `fall` (one direction lowered after N
  datagrams, unreported), the longest datagram each way, and two shared checks used by three drivers.
- `crd-quic-peer` reports `pmtu`, `pmtu_probes_lost`, `black_holes`, `peer_max_udp_payload` and takes
  `--receive-bytes`; the ngtcp2 peer reports its `pmtu` and the `max_udp_payload_size` it read from Cerid.
- Cases: `quic_interop.py` +20, and every case now checks both ends' sizes; `http3_interop.py` +2 per peer and side;
  `webtransport_interop.py` +2 per peer and side. Unit: each end's slot bounds the other's search; receive_bytes
  refusals; a two-way transfer over a path falling toward one end; probe and anti-deadlock timeouts over full storage.
- A size-aware black hole rule (clear suspicion only by an acknowledgement as large as the loss) was tried and
  reverted: it missed nothing the old rule missed, and it took 10% random loss with mixed sizes for a black hole.

## Evidence (linux-gcc-public-checks, WSL)

- `crd-net-tests [net]`: 666 cases, 86.0 million assertions. Corpus replays of `quic-recovery`, `quic-connection`
  and `quic-pmtu` pass; `crd-fuzz-net-quic-recovery` (linux-clang-fuzz) ran 823,065 inputs in 240 s clean after its
  oracle learned the new rule (the first run's oracle failure is what found the anti-deadlock case).
- `crd-net-quic-interop-ngtcp2` 134/134: Cerid finds 1350, 1280 and 1472 to the byte; ngtcp2 reaches 1342, 1232 and
  1444, its fixed probes computed and checked exactly; behind a 1250-byte slot ngtcp2 reads 1250 and stops at 1232;
  falls toward ngtcp2 to 1300 and 1252 are found as black holes (about 400 datagrams dropped first) and searched to the
  byte.
- `crd-net-http3-interop-nghttp3` 70/70, `-quiche` 58/58, `crd-net-webtransport-interop-quinn` 94/94 (the Cerid-to-Cerid
  fall case 8 of 8 alone), `-go` 34/34. quic-go advertises `max_udp_payload_size` 1452, which bounds Cerid's search.
- `crd-net-qmux-interop-crate` 213/214 once: `cerid client ... 0x262144 client closes 0 4 stops` (QMux over TLS, code
  this batch did not touch), the crate saw 3 of Cerid's 4 STOP_SENDING before the close; 80 reruns of that case pass.
  Open: find why the fourth can trail the CONNECTION_CLOSE.

## Next

ICMP Packet Too Big as a search hint (RFC 8899 section 4.6, `IP_RECVERR`), validated against the probe it quotes; then
BBRv3.
