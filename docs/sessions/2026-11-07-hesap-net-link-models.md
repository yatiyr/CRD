# 2026-11-07: loss and delay models of a simulated link, against netem

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 107.

> Superseded the same day: the netem-derived code this records is deleted ([session](2026-11-07-no-gpl-derived-code.md)).

## CI first

Every failure in 42319502866 and 42328203363 is at a commit older than its fix: crd-ci-platform (67e30587), test_clock on
clang and under UBSan (b565a3b6). The rest is the registered mikktspace oracle. 42343637429 (229b06b2) is still pending, so the
threaded verification report on win-asan is still open.

## What landed, capability `numerics.link-models`, no allocation

- `link_models.hpp`, `src/link_models.cpp`. Philox and the normal sampler are read as headers from hesap-stats and erfc from
  hesap-special, linking neither, so crd-net stays off crd-jobs. crd-math moves to crd-hesap-net's PUBLIC links.
- netem's models on chance arithmetic: `link_chance` (iproute2's rint), `CorrelatedDraw`, `LossModel` (random, Gilbert-Elliott,
  four states), `netem_delay` (tabledist), `SlotGate`. One deliberate difference: two chances of a half round to 2^32, netem's
  u32 sum wraps and its chain sticks, here that is exactly one and works.
- `gilbert_from_rate_and_burst` and `_and_correlation`: a simple Gilbert chain by what it means.
- `DelayProcess`: a Gaussian AR(1) through each shape's quantile (uniform, normal, log-normal, Pareto, netem's mix), so the
  law is exact at any correlation and `deviation_ns` is the standard deviation in every shape, between a floor and a ceiling.

## Evidence

- `crd-hesap-net-tests` `[link-models]`, 14 cases: fixed point by hand; each machine's transitions on scripted words with a word
  on every threshold; stationary rates and burst lengths in closed form; table arithmetic by hand; every shape's KS distance
  from its own CDF at correlation 0 and 0.5; lag-one by Sheppard's formula; digests of every decision and rounded delay. Clean on
  win-debug, linux-gcc-debug, linux-gcc-asan and linux-clang-tsan, with one digest on all four.
- `crd-hesap-link-netem-tests` (linux-gcc-public-checks; Linux 6.12's `sch_netem.c`, cut verbatim; iproute2 6.12.0's generators,
  built and run): every decision, delay, slot and word consumed, bit for bit, on Philox and on threshold words, on the three
  tables and uniform, with correlation. The analytic shapes hold netem's tables up to their scale, within a table step and KS.
- netem's correlated word at `loss 1%` and 25% correlation loses 0.000125% on the same stream; the chain loses 1.0012%.
- `crd-fuzz-hesap-link`, 26 seeds: 3,572,768 libFuzzer executions clean under ASan and UBSan; replay clean on win-debug. The first
  run's one finding was the target's own oracle: a table with no deviation opens a slot and draws no word.
- 42 teeth, all caught, with the netem oracle out of the loop. Two first survived (the correlated weight's plus one, a word on
  the bad state's exit), so the scripted cases gained a word for each.
- [Board](../bench/2026-11-07-hesap-net-link-models-vs-netem.md): level to 1.04x on the loss machines, 1.60x on the table delay,
  after a branch on the entry's sign had put it 2.1x behind.
- clang-tidy clean on win-debug's files and on the peer lane's two.

## Next

RaptorQ per RFC 6330; FlexFEC and RED against libwebrtc's reader, writer and splitter; then the audits the row names.
