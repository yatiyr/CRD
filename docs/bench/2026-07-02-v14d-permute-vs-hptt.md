# 2026-07-02 — v14-d tensor permute vs HPTT (single-thread head-to-head)

- **Machine/config:** i9-14900K, WSL2 Ubuntu 24.04, `taskset -c 4` (one P-core thread), median of 10.
  Both sides g++ 13.3 `-O3 -march=native`. HPTT = the tensor-transpose gold standard
  (`external/hptt`, SHA 9425386, `hptt::ESTIMATE` plans, alpha=1) — baselines from
  `external/PEER_ORACLES.md`. GiB/s basis = 8 bytes/element on both sides.
- **Harness:** `scripts/run_permute_bench.sh` (`scripts/bench_permute.cpp`); HPTT's column-major
  cases mapped to row-major ({1,0} and full reversal are self-symmetric; col-major 512³ {2,0,1} ≡
  row-major {1,2,0}). Correctness gates: 10 cases / 1,389 asserts bit-exact (all 24 orders of
  5×6×7×8, tile edges, sliced/flipped sources, NumPy corpus) green win-debug + linux-gcc through
  every optimization step.

## The board (1T, f32; lower ns/elem is better)

| Case | Cerid | HPTT 1T | Ratio |
|---|---|---|---|
| 2D 4096×4096 {1,0} | **0.4949 ns (16.16 GB/s)** | 0.5505 (13.53) | **1.11× WIN** |
| 4D 64⁴ {3,2,1,0} | **0.4180 ns (19.14 GB/s)** | 0.4849 (15.37) | **1.16× WIN** |
| 3D 512³ {2,0,1} | **0.5962 ns (13.42 GB/s)** | 0.7828 (9.52) | **1.31× WIN** |

**Verdict: full-board single-thread crush of HPTT** (1.11× / 1.16× / 1.31×), with zero planning cost
(HPTT ESTIMATE plan ≈ 0.02 ms/call amortized; Cerid needs no plan) and bit-exact determinism by
construction. The multithreaded row (HPTT 8T = 46–58 GiB/s, DRAM-bound) lands with the
deterministic-partition increment.

## Levers (each measured; one refuted)

1. **NT (streaming) stores: REFUTED at 1T** — regressed 4096² 0.598→0.718 and 512³ 0.795→0.897
   (one core can't outrun the regular-store LLC write-back path on Raptor Lake/WSL2). Machinery kept
   behind a disabled gate for the MT increment, where aggregate NT is the classic win. (The agent's
   "HPTT streams" premise didn't survive measurement — SANITY #5.)
2. **Src-locality-ordered odometer** — outer dims iterated innermost-by-ascending-|src-stride|, so
   consecutive planes move the scattered stream by its smallest step (page/TLB reuse). The 4D
   reversal lever: 0.696 → 0.442.
3. **Stride-aware tile edge** — 64 when tile columns are near (≤64 KB apart) or the plane is a single
   tile; 32 when 64 page-scattered columns would thrash the dTLB (512³: 0.885 → 0.596).
4. 64-byte alignment for all tensor storage (Tensor::resize) — aligned SIMD + NT legality.

## Increment 2 (2026-07-02, same day): the deterministic MT pass — 8T head-to-head

MT engine: macro-tasks = (outer odometer × 8×8-tile super-blocks over BOTH transpose dims), disjoint
dst rectangles per task ⇒ bit-identical for ANY worker count by construction ({1,2,4,8,16} gate in
`test_permute.cpp`). Levers measured: **NT stores WIN in the MT regime** (regular-store MT: 30–34 GB/s;
NT: 42–51 — the 1T refutation inverts under aggregate traffic, as predicted); per-task `_mm_sfence()`
(the jobs counter's release ordering does not cover NT stores); **MT tile rule INVERTED from 1T**
(near columns→32: per-tile read-stream count under the L2 streamer cap with 8 cores in flight;
scattered→64); conditional next-block prefetch (near-column cases only; +10% on 2D, regresses
scattered cases — gated). 16T/HT: no gain (DRAM-bound).

⚠ The recorded HPTT 8T oracle baselines (0.1274/0.1441/0.1596) were measured on a FRESH machine
state; hours of benching later the box runs ~25–45% slower on both sides. The honest comparison is
the **matched-state A/B** (alternating runs, same minute, same pinning, 2 rounds):

| Case (8T, matched state) | Cerid | HPTT | Verdict |
|---|---|---|---|
| 2D 4096² {1,0} | 0.1755–0.1844 ns/elem | 0.1869–0.1915 | **1.04–1.09× WIN** |
| 4D 64⁴ {3,2,1,0} | 0.1621–0.1868 | 0.2344–0.2389 | **1.27–1.45× WIN** |
| 3D 512³ {2,0,1} | 0.2360–0.2433 | 0.1858–0.2042 | **0.79–0.84× LOSS — OPEN (SANITY #9)** |

(A/B harness: `build/crd_permute_ab.sh`. Cerid additionally pays zero planning cost vs HPTT's
ESTIMATE plan per shape.) The 3D row is an open bug: HPTT wins the 1MB-column-stride case at 8T;
candidate levers for the next pass: per-thread column-band ownership across planes (page reuse without
task-boundary churn), 4-column-fused reads in the microkernel for scattered strides.

## Increment 3 (2026-07-02, the 3D-row crush pass)

**Rectangular MT tiles** (tall-in-a 256×64 on scattered columns: 1 KB sequential per column visit
instead of 256 B, 4× fewer cold-miss rounds) — the real lever: 4D 0.162→0.137 best, 3D 0.236→0.187
best. **Fused-outer plane-groups** (sequential column reads across consecutive planes) built and
MEASURED NO-GAIN, reverted: the microkernel's NT-write-friendly ia-outer order re-fragments reads
inside each tile; inverting fragments the writes — the in-tile loop-order tension is the named lever
for any future 3D pass.

**Final matched-state A/B (4 rounds today, ±10-15% machine variance on BOTH sides):**

| Case (8T) | Cerid range | HPTT range | Verdict |
|---|---|---|---|
| 2D 4096² | 0.156–0.191 | 0.146–0.192 | **parity** (rounds split) |
| 4D 64⁴ | 0.137–0.181 | 0.151–0.239 | **WIN (every matched round)** |
| 3D 512³ | 0.187–0.243 | 0.152–0.204 | **~0.82× LOSS — OPEN** |

3D remains the one open row (SANITY #9): HPTT wins the 1MB-column-stride shape at 8T in every
matched round. Pinned lever: an ib-outer tile variant with write-combining-aware dst staging
(a small dst bounce buffer per tile written out linearly) — resolves the loop-order tension at the
cost of one L1-resident copy. 1T remains a full 3-case crush (1.11×/1.16×/1.31×).

## Increment 4 (2026-07-03): the 3D row CLOSED — full-column staged strips

**The winning kernel** (`permute_tile_staged_f32`): scattered-column MT tiles become FULL-COLUMN
strips (512×32 = exactly the 64 KB stage): reads run ib-outer through the 8×8 microkernel into an
L1/L2-resident stage — every 1MB-apart column is read EXACTLY ONCE per plane as one sequential 2 KB
run (8 concurrent streams, not 64) — then the stage streams to dst linearly (full-line NT). The two
prior attempts (fused-outer plane groups; 256×64 staged) failed because columns were still revisited
across ta-bands; full-a strips remove the revisits entirely. Gate coverage: the MT moat test crosses
the stream threshold (12 MB), so the staged kernel is bit-gated vs serial across {1,2,4,8,16}.

**Final matched-state A/B (2 rounds, 2026-07-03):**

| Case (8T) | Cerid | HPTT | Verdict |
|---|---|---|---|
| 2D 4096² | 0.1527–0.1688 | 0.1743–0.1798 | **WIN both rounds (1.03–1.18×)** |
| 4D 64⁴ | 0.1412–0.1600 | 0.1801–0.2344 | **WIN both rounds (1.28–1.47×)** |
| 3D 512³ | 0.1667–0.1752 | 0.1557–0.1793 | **PARITY (rounds split; ranges overlap)** — was a consistent 0.82× loss |

**Verdict: no losing rows remain.** 8T = 2 wins + 1 parity-within-noise; 1T = 3/3 crush
(1.11×/1.16×/1.31×). Cerid additionally pays zero planning cost and carries the {1..16}
bit-identity guarantee HPTT does not.
