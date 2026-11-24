# 2026-11-24: NET-PATH, the provider's idle memory and a closed stream's buffers

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Memory bullet of
[the QUIC connection](../design/network-foundation.md#quic-connection) and the Slots bullet of
[S40](../design/network-foundation.md#quic-stream-set). Follows [the held memory](2026-11-24-net-path-held-memory.md),
whose first next item this is. Everything ran on this PC (WSL2).

**CI first:** nothing to fix: CI 45401974013 (4f83e628) has no failed job; 45433736835 (f73663f9) had not started.

## What landed

- AWS-LC frees its handshake configuration when the handshake ends (`SSL_set_shed_handshake_config`). Every
  `SSL_*` call in `tls.cpp` was checked for a read of `ssl->config`; two read it. `QuicHandshake` keeps the limits
  of `SSL_quic_max_handshake_flight_len` from `init()`, and `set_version` (`SSL_set_session_id_context`) on an ended
  handshake now sets only the next `init()`'s version, so the ended handshake's tickets keep its own.
- A level's outgoing CRYPTO buffer is taken when data is first written there and freed by the `take()` that drains it.
- `QuicStreamSet::trim()` gives back the buffers of every slot no open stream holds. The connection's trim calls it
  when no ack-eliciting packet has gone either way for three PTOs (sent packets alone would trim a busy receiver).
  `closed_buffers()` makes the trim due: a stream closed holding buffers since the last trim.
- The board program takes its stream events, so a finished stream's slot closes as an application's does.

## Evidence

- Tests: an ended handshake, with and without mutual authentication, answers the receive limits, protocol, policy,
  exporter, peer parameters and a client's later ticket, refuses Handshake data without an alert, and resumes. Streams
  back to back for 3 s keep their buffers at the receiving end; 2 s idle frees them with no timer left; an open stream
  keeps its own; one closed after a trim goes at the next. Teeth: sent packets alone, or no `trim()`, fail the case.
- linux-gcc-debug: all 313 `crd-crypto-tests` and 1,199 `crd-net-tests` cases and 19 guard CTests pass. linux-gcc-asan:
  96 handshake and TLS cases, 256 connection, client, server and host cases, and 232 stream, QMux, HTTP/3,
  WebTransport and channel cases pass. ngtcp2, picoquic and msquic interop and the board check pass.
- linux-clang-fuzz: four corpora replay clean; libFuzzer ran QUIC handshake 4,164,676, TLS 125,079, connection 5,478
  and stream set 643,607 times, no artifact. LLVM 22.1.8 clang-tidy is clean on every touched source.
- massif, 300 held servers after 4 KiB: AWS-LC fell from 5.4 to 2.9 KiB per connection (its 128 KiB entropy pool,
  the same at 100 and 300 held, aside); the drained buffers went (`BUF_MEM` 1,005 to 77 bytes). The rest (connection
  and handshake state, 1-RTT placeholder AEAD contexts, the session) goes only with the connection, which the exporter
  and a client's later tickets need. The stream buffers (3.7 KiB) are gone.
- Memory board (3 rounds, against f73663f9), KiB per held connection: server 32.1 against 35.1, client 32.3 against
  35.6; msquic 32.3 and 73.1. After 4 KiB (7 rounds): server 32.3 to 32.7 against 39.5 to 40.0; msquic's server
  16.8 to 60.0 over 9 rounds (median 38.5), both ends 92.7 at best against 65.0.
- Handshakes per CPU-second 4,741 against 4,648 (5 rounds); bulk 589 against 585 MiB per CPU-second at 1 MiB windows
  and 446 against 429 at 64 KiB (3 rounds each). Bursts apart by more than three PTOs were not measured.

## Next

1. CI: the first finished run containing da311710 (MSVC, clang-cl, ARM64, TSan, win-asan), and a fuzz host-check of
   `net-quic-connection`.
2. The idle-trim board row (bursts apart by more than three PTOs): what retaking buffers costs.
3. A held server end below msquic's best rounds: the 25 KiB connection object is the largest part left.
