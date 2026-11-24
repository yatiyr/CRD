# 2026-11-24: NET-PATH, a held connection's handshake keys and idle stream buffers

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Memory bullet of
[the QUIC connection](../design/network-foundation.md#quic-connection) and the Slots bullet of
[S40](../design/network-foundation.md#quic-stream-set). Follows [the growing tables](2026-11-24-net-path-growing-tables.md),
whose first next item this is. Everything ran on this PC (WSL2).

**CI first:** nothing to fix. CI 45401974013 (4f83e628) had 22 jobs green and 10 running, none failed.

## Where the server's 50.3 KiB went

massif on a server holding 300 connections, plus `ptype /o` of the 25,776-byte `QuicConnection`: the object itself
25.2 KiB, a slot's two stream buffers 8.3, sent storage, ledger and recovery 7.6, AWS-LC 6.1, the rest 3.1. Of the
object, 5.6 KiB were the Initial, Handshake and 0-RTT keys, and the board's stream carries no data at all.

## What landed

- `QuicPacketProtection` takes those three levels' keys as one block from its allocator at `start()`, and wipes and
  frees it once all three are discarded; a keyed connection takes none, and a refused block refuses the start. The
  connection counts the block in `buffer_allocations()`.
- A stream slot's buffers come through `quic_hold()` when its stream first has data, `first_bytes` each; a stream
  with only a FIN or a reset takes none. An open no longer allocates, so `QuicStreamError::NoMemory` is gone.

## Evidence

- Tests: the block in every discard order, a restart, the destructor, a keyed start and a refused start; the
  connection's start refused; a stream's first block refused, then growth refused; no buffer for open streams
  without data. linux-gcc-debug: all 1,197 `crd-net-tests` cases and 211 CTests (guards and net interop) pass;
  linux-gcc-asan: the packet, stream, QMux, client, server and 189 connection cases pass.
- linux-clang-fuzz: the five corpora replay clean; libFuzzer ran packet 348,410, stream set 572,410, stream 527,142,
  QMux 45,056 and connection 10,252 times with no artifact. LLVM 22.1.8 clang-tidy is clean on every touched source.
- Memory board (3 rounds, against aa1d59a1's build), KiB per held connection: server 35.1 against 50.5, client
  35.1 against 50.5, both ends 70.3 against 101.0; msquic 40.1 (server), 77.8 (client), 117.9 (both).
- Handshakes per CPU-second 4,712 against 4,613 (5 rounds); bulk at 1 MiB windows 587 against 566 MiB per CPU-second.

## Next

1. The 6.1 KiB AWS-LC keeps per server connection after the handshake: list every reader, then free it.
2. The idle-trim board row (bursts separated by pauses longer than three PTOs), named by the last session.
