# 2026-11-07 — HESAP-NET: link models

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [No GPL-derived code](../sessions/2026-11-07-no-gpl-derived-code.md).

## Machine and arms

Intel Core i9-14900K, WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_LINK_VS_NETEM=ON`, `crd-hesap-link-board --rounds 25`: 1,048,576 packets, the best of 25 interleaved rounds, Philox
words included. The host was shared. There is no peer row: netem runs in the kernel behind a qdisc and has no loop to time beside
these, and its laws are compared by `crd-hesap-link-netem-tests`.

## Cost, nanoseconds per packet

| Model | ns |
|---|---|
| Independent loss | 3.34 |
| Gilbert-Elliott | 3.49 |
| Four states | 3.97 |
| Medium grants, four packets a grant, fixed gap | 5.76 |

Every loss setting draws two words a packet, so a stream's position depends only on the packet count; that is about a
nanosecond and a half of each row, beside the deleted first version's one word (1.75 ns without Philox, in the [superseded
board](2026-11-07-hesap-net-link-models-vs-netem.md)).

## The exact laws

| Shape | Correlation 0 | Correlation 0.5 |
|---|---|---|
| Uniform | 43.6 | 43.9 |
| Normal | 28.5 | 28.6 |
| Log-normal | 36.8 | 36.7 |
| Pareto | 76.0 | 76.3 |
| Pareto-normal | 77.9 | 78.1 |
| Table, five entries | 45.7 | 46.0 |
