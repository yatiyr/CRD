# 2026-06-19 — FFT M15: orchestrator campaign + 1024 gather fusion banked

## What shipped (banked, default-on)

**1024 gather fusion** for the Bailey four-step FFT (f32 + f64, forward, N where a 1024 sub-FFT
appears — i.e. 1M and the 1024-axis of 2M). Default-on; disable via `CRD_FFT_DISABLE_FUSED`.

The gather-fused stage-1 codelet (`codelet32_stage1_fused_32x32_gather`) reads the four-step input
strided **directly** (rs=n2 for P1, rs=n1 for P2) — eliminating the gather memcpy **and** the scratch
round-trip (the phase-separated path moves ~32 MB; this ~16 MB) — then stage-2 → scratch. The bbuf is
**bit-equivalent** to the phase-separated path (verified).

| dtype | N | result |
|---|---|---|
| f32 | 1M | **1.083×** vs phase-separated (0.69 → 0.77× MKL) |
| f64 | 1M | **~1.086×** |
| f32 | 2M | small bonus |
| f32/f64 | 4M / 8M / 16M | untouched (fusion does not fire — 2048/4096 sub-FFTs) |

Correctness: maxrel ~2.8e-7 (1M) vs MKL; bit-equivalent intermediate; Windows win-debug FFT suite
green (138 assertions / 22 cases).

## What was measured and rejected (the full optimization surface)

The isolated CRD batched-1024/2048/4096 codelet was measured **faster than MKL** (1.53× / 1.42× /
1.17×). So the remaining f32 1M/2M gap (~0.80× MKL) is **orchestration/dataflow**, not codelet quality.
Every incremental capture of that gap was measured and rejected:

| mechanism | result | why |
|---|---|---|
| 2048 / 4096 gather fusion | severe regression | DRAM-bound working set — strided codelet loads lose to the dedicated prefetched gather |
| high-radix / direct Stockham | rejected | the four-step radix-32 codelet already wins |
| blocked gather | rejected | gather already ~50 GB/s |
| separate-phase prefetch / double-buffer | rejected | gather is throughput- not latency-bound |
| C2 twiddle table | weak (1.08×) | the twiddle is not recurrence-bound |
| C3 blocked twiddle-store (alone) | weak in context (1.05×) | the store is already overlapped with the read/recurrence |
| C4 full per-tile orchestrator (BB sweep) | parity / regress | the engine's NT-twiddle is bw-sensitive — costly at BB=8 |
| C5 split-copy | regress | the csc→aggregate copy (~0.6 ms) exceeds the tiling benefit (~0.43 ms) |
| C6 direct aggregate emit | regress | the strided aggregate store penalty exceeds the tiling benefit |

**Converged diagnosis:** the BB=8 codelet cache-tiling benefit is real but **un-capturable
incrementally** — the BW=128 NT-twiddle requires the n2v·128 scratch layout, and producing that from
BB=8 chunks costs (small-bw-twiddle | copy | strided-store) ≥ the benefit every way. The orchestration
is at a **hard local optimum** with the gather fusion.

## The honest standing + future (M16)

The incremental four-step capture mechanisms are exhausted. The CRD codelet is strong (measured faster
than MKL in isolation); the remaining f32 1M/2M gap is a **fused-phase kernel/dataflow rewrite**
problem, not a small-patch problem. A net win past ~0.80× MKL requires a fundamentally different kernel
where the leaf, the inter-stage twiddle, and the transpose store are **one scheduled program** per
1024-axis tile (MKL-class fused codegen) — so there is no separate-phase layout to reconcile. The
C4–C6 probes have precisely mapped what that kernel must avoid (the bbuf/scratch round-trips **and** the
layout-reconciliation cost). That is **M16**.

Probes live under `build/m9_sanity.cpp`, `build/m1*`/`m15_*`, `build/gen_subfft_m3.py` (the generator;
`emit_plain_agg` + the gather suffix arg are kept for history but emit no shipping codelet).
