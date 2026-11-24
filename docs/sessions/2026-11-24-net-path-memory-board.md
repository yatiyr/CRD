# 2026-11-24: NET-PATH, memory per held connection on the QUIC board

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Everything ran on this PC (WSL2, i9-14900K, kernel 6.6, THP `madvise`).

## CI first

- CI 45322193716 (5d441aa3), `crd-net-qmux-interop-crate`: in the mixed case over WebSocket, the Cerid client read
  2 of the crate server's 4 unidirectional streams. It reproduces in 16 of 300 runs on two cores (`taskset -c 0,1`),
  every time with Cerid as the client. The crate's close drops WebSocket writes it has not finished, which is why the
  other WebSocket cases with unidirectional streams, resets or stops already hold. The mixed case now holds 1000 ms:
  0 of 300 fail, and the whole test passes.
- udp-ring host-check 45299047960 (6983e336) ran on an Azure host (7.0.0-1017-azure, memlock 8 MiB). All 640 opens
  got every buffer they asked for, from 4 to 32768, both alone and beside four crd-net-tests runs. CI 45238977369's
  refusal still does not reproduce.
- Fuzz host-check 45351689742 ran 5f977e19, which is before c7360990. Its `net-quic-connection` seed 8702 failed
  the transfer oracle on input 5ed387c4, the oracle c7360990 changed. The input passes on HEAD in both arms and is
  now in `corpus-quic-connection`, whose 78 inputs replay clean.
- msquic arm: the connection of 2,000 that never ended does not reproduce in 140,000 connections at 1 MiB windows.
  That is 40 pairs of 2,000, 30 more pinned to two cores, and a three-round handshake board. When a run does not
  end, the msquic end now prints each live connection's state on standard error.

## What landed

- `quic_board.py --workload memory`: the handshake workload with `--hold 1`. The client keeps each connection open
  and idle once its exchange ends, then closes them all once every exchange has ended. Each end reports `peak_live`,
  and the check requires it to equal `connections` at both ends. The cost is each end's ru_maxrss slope from 200 to
  2,000 held connections. Both board programs take `--hold`. msquic's end keeps its live connections in a list under
  its lock, and a connection leaves the list before its handle closes.

## Evidence (three rounds, best and median; 1 MiB windows, 64 KiB the same within 0.5 KiB)

| arm | KiB per held connection, both ends | median | server | client | server peak MiB at 2,000 | client peak MiB at 2,000 |
|---|---|---|---|---|---|---|
| crd-net | 517.3 | 517.3 | 258.6 | 258.7 | 516.8 | 516.2 |
| crd-net, no offload | 516.4 | 516.6 | 258.0 | 258.4 | 515.2 | 515.8 |
| msquic | 112.2 | 112.8 | 43.2 | 69.0 | 99.2 | 148.2 |

Why the gap: each crd-net connection allocates and zeroes its full capacity when it is built, sized by
`QuicConnectionConfig`. Measured with sizeof and the defaults:

- `QuicConnection` itself: 25.0 KiB (packet protection 10.1 KiB, path control 4.4 KiB)
- crypto buffers: 96 KiB (3 levels, 2 directions, 16 KiB `crypto_bytes`), plus 18 KiB of their bitmaps
- sent entries: 47.8 KiB (3 spaces x 136 x 120 B)
- sent frames: 40 KiB (1,024 x 40 B)
- held packets: 11.5 KiB
- recovery: 2.8 KiB

That is 241 KiB; TLS and stream state make up the rest. msquic's sources (2.6.4) do it differently:

- one TLS send buffer for every level, 4 KiB on a client and 8 KiB on a server (`quicdef.h`);
- a crypto receive buffer that grows as data arrives (`crypto.c`);
- sent-packet metadata taken from a pool as each packet is sent (`sent_packet_metadata.c`).

Also checked here: the board check passes in `linux-gcc-public-checks` 20 of 20 times through CTest and 10 of 10
pinned to two cores, and the 19 `crd-no-*` and `crd-check*` guards pass. The driver passes `--hold` only to a
holding client, so a `--baseline` build from before this change still runs.

## Next

Bring memory per connection down to msquic's, then run the board again:

- grow the crypto buffers on demand and release each space's when it is discarded (RFC 9001 section 4.9);
- grow the sent entries and frames with the flight.
