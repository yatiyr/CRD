# 2026-11-24: NET-PATH, sent-packet storage, the frame ledger and the held ring grow with the flight

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: the Memory bullet of
[the QUIC connection](../design/network-foundation.md#quic-connection). Follows
[the preferred IDs](2026-11-24-net-path-preferred-ids.md), whose next item this is. Everything ran on this PC (WSL2).

**CI first:** nothing to fix. Every job of CI 45358207939 (6c5c06fe) passed, win-asan included.

## What landed

- A space's sent-packet storage starts at 22 entries. After each recorded packet it keeps room for the reserve, two
  probes and the next packet, and it doubles up to `sent_packets` plus the reserve, so `QuicRecovery` answers as a
  full storage would. `swap_storage` moves the entries. Discard frees Initial's and Handshake's storage; a closed
  slot's goes back to its first size, taken at construction. A refused growth keeps the smaller storage.
- The frame ledger starts at 64 records and doubles up to `sent_frames`. A token is its first record's number modulo
  2^31, so it survives a move. A refused growth closes the connection with INTERNAL_ERROR, and the close is sent.
- Three PTOs after the last ack-eliciting packet, with nothing in flight, grown tables shrink; `next_timer_ns()`
  names that time. The held ring exists only while a packet waits for its keys.

## Evidence

- Tests cover:
  - a 2 MiB stream (lossless and at 2% loss) that ends byte-exact, then shrinks back with no timer left;
  - 96 streams with 256-byte windows at 2% loss, which move the ledger while packets are lost;
  - a pinned stream that grows slot 1's storage, which returns to its first size when path ID 2 reuses the slot;
  - refusals of the storage, the ledger and the held ring;
  - `swap_storage` across a wrapped ring. A move operation was added to the recovery fuzzer.
- linux-gcc-debug `crd-net-tests`: all 903,142,026 assertions pass, and so do the 19 `crd-no-*`/`crd-check*` guards.
  linux-gcc-asan: the 273 connection, recovery, early-data and multipath cases pass. All 12 net interop CTests pass.
- linux-clang-fuzz: the four net corpora replay clean. libFuzzer ran recovery for 387,289 and then 254,897 runs, and
  connection for 10,228 and then 10,157 on the final code, with no artifacts. LLVM 22.1.8 clang-tidy is clean on
  every touched source.
- Board against 4f83e628. KiB per held connection, both ends: 100.7 (50.3 server, 50.3 client) against 289.0;
  msquic 111.8 to 113.8, with its server at 37.5 to 50.3. Handshakes per CPU-second: 4,625 against 4,208.
- Bulk MiB per CPU-second: 590 against 591 (medians 584 and 587). The first build was 1 to 2.5% slower because
  `read_held()` freed an absent ring on every datagram and the trim timer ran during flights; both are fixed.
- In two memory runs, one of msquic's 2,000 connections never got established at either end.

## Next

1. The server end's 50 KiB per held connection against msquic's 37.5: find what makes it up and cut it.
2. A board row for bursts separated by pauses longer than three PTOs, each of which regrows the tables.
