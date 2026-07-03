# 2026-07-02 — v14-b elementwise + broadcasting engine vs numpy/torch

- **Machine/config:** i9-14900K, WSL2 Ubuntu 24.04, single pinned core (`taskset -c 4`), 10 reps.
  Cerid: g++ 13.3 `-O3 -march=native -DCRD_SIMD_TARGET=2` (Vec8f/Vec4d). Peers single-threaded:
  numpy 2.4.6 (`out=` preallocated — no allocation overhead measured), torch 2.12.0+cpu
  (`torch.set_num_threads(1)`, `out=`).
- **Harness:** `scripts/run_elementwise_bench.sh` (`scripts/bench_elementwise.cpp` +
  `scripts/bench_elementwise_peers.py`). Correctness gates: NumPy **bit-exact** corpus
  (`scripts/v14b_elementwise_corpus.py` → `tests/hesap-tensor/ref_elementwise.inc`) — 4-D broadcast
  × all six binaries (incl. div-by-zero→inf), strided/sliced sources, rank-0 scalars, sign-bit
  neg/abs (±0 + NaN payloads), compare + three-way-broadcast where, f32↔f64 casts. Suite: 22 cases /
  104,415 asserts green (win-debug /WX · win-asan 0 errors · linux-gcc -Werror).

## The board (ns/element; lower is better)

| Case | Cerid | numpy | torch | Verdict |
|---|---|---|---|---|
| contiguous add f32 1M | **0.209** | 0.220 | 0.605 | 1.05× numpy (the memory-bound ceiling — both stream 12 B/elem) · **2.9× torch** |
| outer broadcast (4096,1)×(1,4096) mul f32 16M | **0.213** | 0.320 | 0.359 | **1.50× numpy · 1.68× torch** |
| row broadcast (2048,2048)+(2048,) add f64 4M | **0.583** | 0.742 | 0.868 | **1.27× numpy · 1.49× torch** |
| strided-row (::2) mul f32 4M | **0.209** | 0.328 | — | **1.57× numpy** |

**Verdict:** full-board win — the P1 splat-run lever (stride-0/unit-stride inner runs SIMD'd directly,
no generic-iterator overhead) crushes the broadcast/strided rows; the pure-contiguous row sits at the
DRAM ceiling with a slight edge. Zero losses.

## Levers (recorded)

- Dimension collapse across ALL operand streams first (jointly-contiguous dims fuse) → same-shape dense
  cases become one flat SIMD loop.
- Inner-run dispatch: {1,1} / {1,0} / {0,1} stride pairs get dedicated SIMD loops with hoisted splats;
  generic strides fall to scalar (P2).
- Neg/Abs as sign-bit XOR/AND (bit-exact vs np.negative/np.abs incl. NaN payloads; auto-vectorizes).
- Semantics pinned: Min/Max = IEEE hardware (second-operand-on-NaN); NumPy's NaN-propagating variants
  deferred until a consumer needs the split (documented in elementwise.hpp).
