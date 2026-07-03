# 2026-07-02 — v14-c reductions: Tier-D fixed-tree + Tier-R binned vs ReproBLAS

- **Machine/config:** i9-14900K, WSL2 Ubuntu 24.04, `taskset -c 4`, median of 10, g++ 13.3
  `-O3 -march=native`. ReproBLAS v2.1.0 baselines from `external/PEER_ORACLES.md` (same machine, same
  pinning, same `sin(2π(i/N−0.5))` cancellation workload, fold=3).
- **Harness:** `scripts/run_reduce_bench.sh` (`scripts/bench_reduce.cpp`). Correctness gates
  (`tests/hesap-tensor/test_reduce.cpp`, green win-debug + win-asan + linux-gcc): Tier-D values vs
  analytic references; **the {1,2,4,8,16} moat** (parallel ≡ serial, bit-identical, f32+f64); **Tier-R
  bit-identity under forced REPARTITION** ({3,7,16} chunks × forward/reverse merge) **and full element
  shuffle**; integer exactness; accuracy ≥ naive on cancellation; SR-accumulation seed-reproducibility
  + grid unbiasedness.

## The board (f64 sum, ns/element; lower is better)

| N | naive L2R (contrast) | **Tier-D fixed-tree** | **Tier-R binned (Cerid)** | ReproBLAS rdsum(3) | Tier-R verdict |
|---|---|---|---|---|---|
| 1M | 0.358–0.363 | **0.120** | **0.220** | 0.353–0.360 | **1.60–1.63× CRUSH** |
| 16M | 0.503–0.530 | **0.351–0.371** | **0.466–0.480** | 0.486–0.571 | **1.01–1.23× WIN** |

- **Tier-D** (deterministic {1..16}-bit-identical) runs ~3× the naive serial loop @1M and at the DRAM
  ceiling @16M — this is the default `reduce_sum`, faster than what numpy/torch-class serial loops do
  while carrying the parallel-determinism guarantee they lack.
- **Tier-R** — the reproducible, partition-independent sum — now costs LESS than a plain naive serial
  sum at DRAM sizes: reproducibility is cheaper than non-reproducibility.

## Levers (each measured)

1. Scalar transcription baseline: 2.37 ns/elem — a 6.7× loss (an open bug per SANITY #9).
2. **12 independent binned accumulators** (3 latency-hiding streams × 4 Vec4d lanes), merged at the
   end — legal because the binned representation is partition-independent (the exact property the
   repartition gates prove): 2.37 → 0.357 (parity @1M).
3. **Speculative single-DRAM-pass**: deposit under the previous super-block's index window while
   tracking |max| in the same registers; on the rare violation, restore the 576-byte accumulator
   snapshot and redo the block from L2. Kills the separate |max| pre-pass read: 0.357 → **0.220** @1M,
   0.578 → **0.47** @16M. Bit-identical by the same property.
4. Tier-R is a faithful fold-3 transcription of ReproBLAS v2.1.0's core (dindex/dmbins/dmdupdate/
   dmddeposit/dmrenorm/dmdmadd/ddmconv — external/reproblas SHA dfb8150), so the reproducibility
   guarantees carry over by construction, then Cerid's orchestration wins the clock.
