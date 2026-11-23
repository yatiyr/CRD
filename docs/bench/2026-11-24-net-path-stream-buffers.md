# 2026-11-24 — NET-PATH: the QUIC board with stream buffers grown on demand

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [NET-PATH](../ROADMAP.md#slice-net-path). Session: [stream buffers](../sessions/2026-11-24-net-path-stream-buffers.md).
Method, machine and arms as the [first board](2026-11-23-net-path-quic-board.md) (`quic_board.py --rounds 3`,
`linux-gcc-release` at 1de387ac, WSL2 on the i9-14900K), with crd-net's buffers taken as data needs them (S40).

## Results

| arm | handshakes per CPU-second, 64 KiB windows | median | at 1 MiB | median | 2026-11-23 at 64 KiB | at 1 MiB |
|---|---|---|---|---|---|---|
| crd-net | 3,626 | 3,584 | 3,693 | 3,424 | 3,175 | 915 |
| crd-net, no offload | 3,581 | 3,414 | 3,562 | 3,505 | 2,994 | 968 |
| msquic | 1,508 | 1,466 | failed | | 1,536 | 1,561 |

| arm | MiB per CPU-second, 64 KiB windows | at 1 MiB | 2026-11-23 at 64 KiB | at 1 MiB |
|---|---|---|---|---|
| crd-net | 268 | 287 | 272 | 294 |
| crd-net, no offload | 180 | 181 | 182 | 185 |
| msquic | 980 | failed | 966 | 984 |

## Reading

- crd-net's handshake cost no longer follows the stream window: 3,693 per CPU-second at 1 MiB, four times the 915 of
  2026-11-23 and the same as at 64 KiB; 2.4 times msquic's 1,508 at 64 KiB. Bytes per CPU-second are unchanged
  within the rounds' spread (the buffers reach their limits in the first milliseconds of a transfer).
- The msquic arm at 1 MiB failed both runs in its first round: of 2,000 connections the server saw 2,000 and the
  client reported 1,999 established and ended, and the server waited out its timeout. The 1 MiB rows above come from
  a run of the crd-net arms alone. Why one msquic connection never ends is open (the board's harness or msquic).
