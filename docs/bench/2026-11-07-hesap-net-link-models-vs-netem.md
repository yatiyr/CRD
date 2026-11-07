# 2026-11-07 — HESAP-NET: link models against Linux's netem

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net). Session: [Link models](../sessions/2026-11-07-hesap-net-link-models.md).

> Superseded the same day: the code this measured was copied from netem and is deleted ([session](../sessions/2026-11-07-no-gpl-derived-code.md), [board](2026-11-07-hesap-net-link-models.md)).

## Machine and arms

Intel Core i9-14900K, WSL2 Ubuntu 24.04, one thread, GCC 13.3.0, the `linux-gcc-release` tree with
`-DCRD_BUILD_LINK_VS_NETEM=ON`. The peer, without link-time optimisation and with its loop inside its own unit: Linux 6.12's
`loss_event` and `tabledist`, cut verbatim from `net/sched/sch_netem.c`. Both arms read their words from one array, so the
cost of Philox is in neither. `crd-hesap-link-board --rounds 25`, run twice: 1,048,576 packets, the best of 25 interleaved
rounds. The board fails if an arm's sum of decisions or delays differs from the peer's.

## Cost, nanoseconds per packet

| Model | netem | hesap | Ratio |
|---|---|---|---|
| Random loss, 1% | 1.75 to 1.76 | 1.75 to 1.76 | 1.00x |
| Gilbert-Elliott | 3.95 to 3.98 | 3.82 to 3.84 | 1.03x to 1.04x |
| Four states | 2.21 to 2.22 | 2.12 to 2.13 | 1.04x |
| Table delay, 16 entries, mixed signs | 2.99 to 3.00 | 1.87 to 1.88 | 1.60x |

The table delay gains from one line: netem branches on the sign of a scaled entry, as random as the word that picked it, and
hesap adds its half-scale without a branch. The loss rows are near level because their cost is the word and one compare.

## The exact laws

netem has no peer for these: it draws from a table. These rows are what an exact marginal law costs beside it, Philox and
Box-Muller included, against about 2 ns plus a Philox word for a table.

| Shape | Correlation 0 | Correlation 0.5 |
|---|---|---|
| Uniform | 43.8 to 44.0 | 44.1 to 44.3 |
| Normal | 28.7 | 28.7 |
| Log-normal | 37.1 | 37.1 |
| Pareto | 76.4 | 76.5 to 76.7 |
| Pareto-normal | 78.5 to 79.0 | 78.8 to 79.5 |

Every delay is at most 80 ns per packet, on a path that takes microseconds per packet to send.

## What netem's correlation costs

On the same Philox stream, 4,000,000 packets (`crd-hesap-link-netem-tests`):

| Asked | netem's correlated word | The two-state chain |
|---|---|---|
| Loss 1%, correlation 25% | 0.000125% | 1.0012% |

netem averages each word with its last answer, which narrows the draw around one half, so a small chance is almost never
reached. The chain's rate and lag-one correlation are the asked ones at every rate and correlation the unit cases try.
