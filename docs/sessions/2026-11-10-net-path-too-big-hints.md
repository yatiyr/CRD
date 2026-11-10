# 2026-11-10 — NET-PATH: ICMP Packet Too Big hints, and two CI and interop fixes

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S68](../design/network-foundation.md#pmtu).

## CI first

- Run 42774048974 (c5a06d38): win-arm64-debug failed `fuzz_congestion.cpp` on C4324 (the policy and datagram size
  before the 16-byte-aligned `CongestionSlot` left a gap). The slot now comes first, as `test_congestion.cpp`'s
  `Controller`, which the same ARM64 job compiled clean (b2dac68b). x64 MSVC does not warn here, so only the next
  ARM64 run can confirm it.
- The QMux interop's one failure from the last batch (`... client closes 0 4 stops`, 1 run in 80) is the test's: the
  Cerid end completes in the turn it asks for its last STOP_SENDING and its CONNECTION_CLOSE is immediate (RFC 9000
  section 10.2), so a frame still queued goes nowhere. The plain TLS case now holds a second like its WebSocket,
  HTTP/2 and HTTP/3 variants; the ten cases with stops passed 25 times each, 250 runs, none failed.

## What landed

- `ReceivedDatagram::too_big`: a socket's report that a datagram it sent met a smaller hop, its quote in the slot.
  `IoLoop` hands it to `IngressHandler::on_too_big` before admission; `QuicHost` offers it to the connections bound to
  its address (`m_peer_paths`, keyed by address and port, a list per address for several connections to one peer).
- `QuicConnection::path_too_big`: matched only to an unacknowledged 1-RTT packet, by the peer's connection ID and the
  four protected bytes after it, recorded per packet as `QuicSentPacket::tag` in padding the struct already had.
- `QuicPathMtu::on_too_big`: a probe above the limit fails at once and the next probe is the limit; a datagram at the
  size in use falls to the floor and searches up to the limit; a limit under the base or at or above the datagram is
  ignored. Counters `hints`, `hints_ignored`; the host's `too_big`, `too_big_refused`; the loop's `too_big`.
- Found by `fuzz_quic_pmtu.cpp` in 28 s and fixed (e8381c74): a failed base (Error) that a lowered ceiling made equal
  to a size a late probe had proved stayed in Error, so the raise timer probed the size already in use. The minimized
  input is in the corpus; a unit test holds it.

## Evidence

- win-debug `crd-net-tests [net]`: 672 cases, 84.8 million assertions. linux-gcc-public-checks: 673 cases, 88.1
  million assertions; the quic-pmtu, quic-connection and quic-recovery corpora; `crd-net-quic-interop-ngtcp2` 134/134.
- Teeth: matching any tag fails the forged-quote cases; walking only the first connection of an address fails the
  host case.
- linux-clang-fuzz: `net-quic-pmtu` 3.23 million executions in 291 s clean, `net-quic-connection` 40,465 in 291 s
  clean.

## Next

The Linux socket half: `IP_RECVERR`/`IPV6_RECVERR`, reading the error queue after an ICMP error, dropping the other
ICMP errors without failing a send or a receive, tested on the loopback (port unreachable) and through a router
namespace sending Packet Too Big where a lane has root. Then BBRv3.
