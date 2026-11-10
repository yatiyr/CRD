# 2026-11-10 — NET-PATH: the Linux socket's ICMP reports, through a real router namespace

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S68](../design/network-foundation.md#pmtu).

## CI first

- Run 42774048974 (c5a06d38), win-tidy: `test_congestion.cpp`'s pacer case named its local constants `kRate` and
  `kBurst`, which the naming rule refuses for a non-static local; they are now `static constexpr` like the other net
  tests' (868ceb65). Its other failure, `test_direct_search.cpp`, is the other owner's. win-arm64-debug's C4324 was
  fixed by b2dac68b; no run with it has finished.

## What landed

- `UdpSocket` on Linux sets `IP_RECVERR`/`IPV6_RECVERR` (`SocketFeatures::too_big_reports`). A wait that sees
  `POLLERR`, or a receive or send failing with an ICMP error's code, marks the error queue; the next `receive()`
  reads it first. Fragmentation Needed and Packet Too Big fill `too_big` slots (hop MTU less 28 or 48, the quote, the
  peer); the rest are dropped. At most 64 reports read and 64 stale errors skipped a call, then the datagrams.
- A send failing with an ICMP code goes again once. `ENOBUFS` (the host dropped the datagram past the socket) counts
  it sent and lost: with reports on, the host says so, and the old Busy retry wedged a QUIC server whose probe a
  narrow link dropped. The router test found it.
- `router_namespace_fixture.hpp`: near host, router and far host in private namespaces (veth pairs, the router's
  routes toward the far host locked to 1280; a veth drops frames over its receiving end's MTU, and an IPv6 route
  needs metric 256 to replace the link's). `[.netns]` cases; CTest `crd-net-router-namespace` under `unshare -Urn`
  (`CRD_BUILD_NET_NAMESPACE_TESTS`, linux-gcc-public-checks, whose CI step now also loads veth).
- `test_quic_client.cpp`'s real-socket host setup is one `RealHosts` helper, shared by the loopback and router cases.

## Evidence

- WSL linux-gcc-public-checks: the five `[.netns]` cases 15 runs in a row clean; `crd-net-tests [net]` 673 cases,
  88.1 million assertions; `crd-net-router-namespace` and `crd-net-quic-interop-ngtcp2` passed under CTest.
- QUIC over the router: the client finds 1252 (IPv4) and 1232 (IPv6) through the reports, the server 1472 and 1452
  back; no black hole, no report refused. Sockets: exact limits, quotes of 520 and 1184 bytes, the dual-stack path.
- win-debug: `[udp],[quic-client]` 24 cases, 8.3 million assertions.
- Teeth: no resend fails the loopback unreachable case; no first read fails the flood case; no `ENOBUFS` count fails
  the host-drop case.

## Next

BBRv3: delivery-rate stamps in `QuicSentPacket`, crd-net linking crd-hesap-net's windowed filter and delivery rate.
