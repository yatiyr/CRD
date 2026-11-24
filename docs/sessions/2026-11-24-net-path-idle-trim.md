# 2026-11-24: NET-PATH, the provider's idle memory and a closed stream's buffers

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Memory bullet of
[the QUIC connection](../design/network-foundation.md#quic-connection) and the Slots bullet of
[S40](../design/network-foundation.md#quic-stream-set). Follows [the held memory](2026-11-24-net-path-held-memory.md),
whose first next item this is. Everything ran on this PC (WSL2).

**CI first:** nothing to fix. CI 45401974013 (4f83e628) has no failed job (8 still running); 45433736835 (f73663f9)
had not started.

## What landed

- AWS-LC frees its handshake configuration when the handshake ends (`SSL_set_shed_handshake_config`). Every
  `SSL_*` call in `tls.cpp` was checked for a read of `ssl->config`; two read it. `QuicHandshake` keeps the limits
  of `SSL_quic_max_handshake_flight_len` from `init()`, and `set_version` (`SSL_set_session_id_context`) on an ended
  handshake now sets only the next `init()`'s version, so the ended handshake's tickets keep its own.
- A level's outgoing CRYPTO buffer is taken when data is first written there and freed by the `take()` that drains it.
- `QuicStreamSet::trim()` gives back the buffers of every slot no open stream holds. The connection's trim calls it
  when no ack-eliciting packet has gone either way for three PTOs (sent packets alone would trim a busy receiver).
  `closed_buffers()` makes the trim due: a stream closed holding buffers since the last trim.
- The board's program now takes its stream events, so a finished stream's slot closes as an application's does.

## Evidence

- Tests: an ended handshake, with and without mutual authentication, answers the receive limits, protocol, policy,
  exporter, peer parameters and a client's later ticket. It keeps no drained buffer, refuses Handshake data with no
  alert, and resumes. Streams one after another for 3 s keep their buffers at the receiving end; 2 s idle frees them
  with no timer left. A stream opened later takes buffers again; an open stream keeps its own; a stream closed after a
  trim is freed by the next. Teeth: counting sent packets only, or no `trim()`, fails the case.
- linux-gcc-debug: all 313 `crd-crypto-tests` and 1,199 `crd-net-tests` cases pass. linux-gcc-asan: 96 handshake
  and TLS cases and 256 connection, client, server and host cases pass.
- massif, 300 held server connections after 4 KiB each: AWS-LC's share fell from 5.4 to 2.9 KiB per connection. What
  is left (the connection and handshake state, the 1-RTT placeholder AEAD contexts, the established session) can only
  go by freeing the connection, and the exporter and a client's later tickets still need it. The stream buffers (3.7
  KiB per connection) are gone.
- Memory board (3 rounds, against f73663f9's build), KiB per held connection: server 32.1 against 35.1, client 32.3
  against 35.6, both 64.4 against 70.7; msquic 32.3, 73.1 and 105.4. After 4 KiB each (2 rounds): server 32.7
  against 39.8, client 33.3 against 40.2; msquic 38.5 to 42.7 and 100.8 to 110.1.
- Handshakes per CPU-second 4,741 against 4,648 (5 rounds); bulk 589 against 585 MiB per CPU-second at 1 MiB
  windows and 446 against 429 at 64 KiB (3 rounds each).
- Interop (linux-gcc-public-checks): ngtcp2, picoquic, msquic and the board check pass. linux-clang-fuzz: the four
  corpora replay clean; libFuzzer ran QUIC handshake 4,164,676, TLS 125,079, connection 5,478 and stream set
  643,607 times with no artifact. LLVM 22.1.8 clang-tidy is clean on every touched source; 19 guard CTests pass.

## Next

1. The idle-trim board row (bursts separated by pauses longer than three PTOs): what retaking buffers costs.
2. CI: read the first finished run containing this batch on the MSVC, clang-cl, ARM64, TSan and win-asan lanes.
