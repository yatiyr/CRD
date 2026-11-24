# 2026-11-24: NET-PATH, CRYPTO buffers grown on demand and given back

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S38](../design/network-foundation.md#quic-streams)'s Storage
and the connection's Memory bullet. Everything ran on this PC (WSL2, i9-14900K).

**CI first:** nothing to fix. Fuzz host-check 45355889672 (554b6f2d) passed; 45377043523 was dispatched on 096b091d.

## What landed

- Both stream buffers gain `release()`, which gives the storage up once nothing is held and keeps the offsets. A buffer
  without storage takes only what needs no room, and `Unusable` is gone. `QuicBufferStore` and `quic_hold()` are now
  the one owner of buffer storage for the stream set and the connection; data past the limit takes no block.
- Each CRYPTO level grows from no storage up to `crypto_bytes` and gives it back once drained, and at once at key
  discard (RFC 9001 section 4.9). A discarded level's packets no longer open, so its data never reaches freed
  buffers; a raw-peer case proves this.
- Refused storage closes with INTERNAL_ERROR: on receipt within the limit, and on sending for a level owing nothing.
  `QuicConnection::buffer_allocations()` and `buffer_bytes()` count both kinds of buffer.

## Evidence

- linux-gcc-debug and linux-gcc-asan: all `crd-net-tests` cases pass, and so do the net fuzz corpora (four under ASan).
  ASan also replayed the 2,883 inputs of this batch's libFuzzer working corpora clean.
- New tests: release at every hold state, `quic_hold`'s doubling, limit and refusals, an established connection
  holding no CRYPTO storage and leaking nothing (keyed too), both INTERNAL_ERROR closes, and the discarded level.
- libFuzzer: stream 724,118 runs, stream set 706,425, connection 9,828, none failing. An earlier 89 s connection run
  reported one UBSan pointer overflow in `QuicPaths::credit`. It does not replay, alone or after its corpus, and it is
  open until host-check 45377043523 ends.
- `linux-gcc-public-checks`: 14 of 15 interop CTests pass, `crd-net-quic-board-check` included. ngtcp2's four
  1280-byte-path x25519mlkem768 cases fail (its search reaches 1200, not 1232). The 0c89649b engine fails 3 of the 4
  too, so the cause came before this batch.
- Memory board (three rounds, 1 MiB windows, interleaved with 0c89649b's build), in KiB per held connection:

| arm | both ends | median | server | client |
|---|---|---|---|---|
| crd-net | 288.7 | 289.1 | 144.2 | 144.5 |
| crd-net at 0c89649b | 516.6 | 517.1 | 258.4 | 258.1 |
| msquic | 117.9 | 134.1 | 50.2 | 67.7 |

- Handshakes: 4,290 per CPU-second, median 4,134 (0c89649b: 3,811, median 3,712). Bulk at 1 MiB windows: 598 MiB
  per CPU-second, median 597 (0c89649b: 606, median 602), within the spread.

## Next

1. Fix the ngtcp2 1280-byte-path regression.
2. Add a connection-level case for a send refused growth while data is owed.
3. Grow sent entries (48 KiB) and frames (40 KiB) with the flight, take held packets (11.5 KiB) only while keys are
   missing, then run the board again.
