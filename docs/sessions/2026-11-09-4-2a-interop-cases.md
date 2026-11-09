# 2026-11-09 — 4.2a: the remaining QUIC interop cases against ngtcp2

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [4.2a](../ROADMAP.md#slice-4.2a). Design: [S50](../design/network-foundation.md#quic-interop). Sources: RFC 9000
sections 6, 10.3 and 14.1, RFC 9001 section 4.6, RFC 9221, RFC 9368; ngtcp2 1.26.0's headers and examples.

CI first: no completed run contains 6474917e yet; 42510394122 and 42527758501 have jobs running, none failing here.

## What landed

Test programs only; no crd-net source changed. `quic_interop.py` grows from 58 cases to 102, each way round in both
versions:

- **Datagrams (RFC 9221)**: 256 of 1000 bytes alone and beside four streams, each echoed in another pattern and held to
  its index, size and single delivery; frames filling a 600-byte `max_datagram_frame_size` exactly, one byte more
  refused by the sender on both stacks; two seeded relays.
- **Resumption and 0-RTT**: two connections in one process (`--connections 2`), plain, 0-RTT accepted and 0-RTT
  refused by the server (Cerid's early data context changes after the first connection, OpenSSL's early data is turned
  off for the second), with 0-RTT datagrams too. The refusal path restarts streams and datagrams on both clients.
- **Version Negotiation**: the client starts in the version the server does not run; one to two packets are counted
  off the wire (the hybrid key share makes a first flight of two datagrams) and both ends finish in the other version.
- **Stateless reset**: the server is killed in mid-transfer and another process takes its port with the same key; the
  client's next packet draws the reset. Only the server side is restarted: a client gives no token for the sequence 0
  ID a server keeps using, so no reset from a client can match.

## Findings

- Both OpenSSL traps were the harness's: `SSL_set_quic_tls_early_data_enabled` fails silently before the QUIC callbacks
  are set, and `SSL_free` without `SSL_SENT_SHUTDOWN` marks the kept ticket unresumable.
- A lost close left the ngtcp2 peer exiting 1 after its idle timeout, an old flake in the lossy cases; idle now counts.
- On the relay a Cerid client delivers less than ngtcp2's paced one: all 256 frames are written and 73 to 86% arrive
  (ngtcp2 buffers four packets ahead of the handshake's end, `NGTCP2_MAX_NUM_BUFFED_RX_PKTS`, so that is the likely
  sink of a first flight overtaking the client's Finished), and 12 to 18% are reported Acked. The rest were delivered
  or lost but declared lost first: S41 reports Lost at the declaration, once, which RFC 9221 section 5.2 allows and
  ngtcp2's `lost_datagram` does too, so a prompt report stays prompt. The case holds delivery at 50%, each datagram
  intact; [NET-PATH](../ROADMAP.md#slice-net-path)'s pacing is measured against these numbers.

## Evidence

`crd-net-quic-interop-ngtcp2` on `linux-gcc-public-checks`: 102 of 102 under CTest, twice (103 s and 104 s). Before the
two OpenSSL fixes the 0-RTT and resumption cases failed on the driver's `early` and `resumed` checks.

## Next

The TCP and TLS path, then the browser endpoints (HTTP/3, WebTransport, WebSocket). NET.4 adds the interop runner and
the other peers.
