# 2026-11-24: NET-PATH, CRYPTO buffers grown on demand and given back

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Design: [S38](../design/network-foundation.md#quic-streams)'s Storage
and the connection's Memory bullet. Everything ran on this PC (WSL2, i9-14900K).

## CI first

Nothing to fix. Fuzz host-check 45355889672 (554b6f2d) passed. Fuzz host-check 45377043523 was dispatched on 096b091d.

## What landed

- `QuicReceiveBuffer::release()` and `QuicSendBuffer::release()` give the storage up once a buffer holds nothing,
  keeping its offsets. A buffer without storage takes only what needs no room: data already read, a FIN or a final size.
  `grow()` gives it storage again, and `Unusable` is gone.
- `QuicBufferStore` and `quic_hold()` are now the only code that takes buffer storage from an allocator. The stream set
  uses them, and its own copies of the growth code are removed. Data that ends past the limit takes no block.
- Each CRYPTO level starts without storage and grows as its data needs, up to `crypto_bytes`. A level gives its storage
  back once it holds nothing, and at once when its keys are discarded (RFC 9001 section 4.9). A discarded level's
  packets no longer open, so its data never reaches the freed buffers; a raw-peer case proves this.
- Refused storage closes with INTERNAL_ERROR: on receipt, for data within the limit, and on sending, for a level that
  owes nothing (no acknowledgement would ever make room).
- `QuicConnection::buffer_allocations()` and `buffer_bytes()` count the stream and CRYPTO buffers. The QUIC suites and
  the connection fuzzer's oracles use them. The server fixture now reads each live connection's count, not a snapshot
  taken at service time.

## Evidence

- linux-gcc-debug and linux-gcc-asan: all 1,189 `crd-net-tests` cases pass (8 Windows-only skips). The 30 net fuzz
  corpus CTests pass on debug, and the four QUIC stream, set, ID and connection corpora pass under ASan. New tests:
  - release at every hold state, regrowth across the ring, and `quic_hold` doubling, its limit and every refusal;
  - an established connection holding no CRYPTO storage and leaking nothing, in both versions, keyed included;
  - both INTERNAL_ERROR closes.
- `crd-fuzz-net-quic-stream` gains a release operation and a `release` seed. libFuzzer ran 150 s on each of three
  targets: the stream (724,118 runs) and stream set (706,425) targets found nothing.
- The connection target reported one UBSan pointer overflow in `QuicPaths::credit` (quic_path.cpp:166). It does not
  replay, either alone or after the whole 1,233-unit working corpus, and `m_active` is never out of range. It matches
  this host's recorded faults, so it is recorded here, not chased. A second run of 235 s (9,828 runs) found nothing.
- Memory board (`quic_board.py --workload memory`, three rounds, 1 MiB windows, interleaved with 0c89649b's build):

| arm | KiB per held connection, both ends | median | server | client |
|---|---|---|---|---|
| crd-net | 288.7 | 289.1 | 144.2 | 144.5 |
| crd-net at 0c89649b | 516.6 | 517.1 | 258.4 | 258.1 |
| msquic | 117.9 | 134.1 | 50.2 | 67.7 |

- No regression. Handshakes, five rounds: 4,290 per CPU-second (median 4,134) against 3,811 (3,712), now that 114
  KiB per connection is no longer zeroed. Bulk at 1 MiB windows: 598 MiB per CPU-second (median 597) against 606 (602),
  within the board's spread. Trains and ACKs per MiB are unchanged.

## Next

The rest of the gap to msquic: sent-packet entries (48 KiB) and sent frames (40 KiB) grown with the flight, held
packets (11.5 KiB) taken only while keys are missing, then the board again.
