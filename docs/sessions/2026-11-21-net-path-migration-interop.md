# 2026-11-21: NET-PATH, migration interop with ngtcp2

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path) (Partial). Design: [S75](../design/network-foundation.md#migration)
(RFC 9000 sections 9, 9.6 and 18.2).

## CI first

Run 44854356220 (d17003bd): every failed job I read (linux-clang-tsan, linux-gcc-asan 2/2, both arm64 ASan
lanes) fails only `crd-repository-tools` and `crd-generated-sources` on the licence manifest's bytes (b0882ee0
already records them), plus the registered TP-5 (MikkTSpace) on linux-gcc-asan. Run 44861565940 (15bea98a) was
still running with no failed job.

## What landed

- `interop_common.Relay` is a NAT toward the server: a server-facing socket per client address, so a client's move
  reaches the server from a new address; `rebind=(after, host)` remaps the client on another port or loopback
  address, and the mapping given up drops what the server still sends there.
- `crd-quic-peer`: a second socket (a client's `--migrate-at`, a server's `--preferred`), `--follow-preferred 0`,
  `--disable-migration 1`, and the final path in the report. `crd-ngtcp2-quic-peer`: two sockets, each packet sent
  by the socket and toward the address of the path ngtcp2 wrote it for, each datagram read with its arrival path,
  `ngtcp2_conn_initiate_migration`, the preferred address offered and selected, the same report fields.
- `quic_interop.py`: 12 path cases each way (rebinding on a port, an address and a lossy path; migration on the
  loopback in both versions, through a lossy relay, after a rebinding, and refused; the preferred address in both
  versions, declined, and followed before the client's own move), held to both reports and the relay's mappings.
- The peer's pre-existing tidy findings (static names, the QMux options' slice) fixed.

## Evidence

- WSL linux-gcc-public-checks: the 24 path cases pass three runs over; then every relay-using suite: ngtcp2 QUIC
  (261 s), nghttp3 and quiche HTTP/3, picoquic, Quinn and webtransport-go, the qmux crate: 7 of 7.
- Teeth: a server that never follows its client failed every ngtcp2-client rebinding and move (8 cases); a client
  that never follows the preferred address failed every Cerid-client preferred case but the declined one (3).
- clang-tidy 22.1.8 clean on `cerid_quic_peer.cpp` (the public-checks tree's database); Allman.

## Next

LEO handover awareness (scheduled handover on a known timetable), then the GSO/GRO, io_uring, USO/URO and RIO fast
paths.
