# 2026-07-05 — v14-z close rows: TBLIS (contraction) + xtensor (elementwise)

> The two contracted peers never measured during the slices (flagged in the v14-z scoreboard
> consolidation). Machine: i9-14900K, WSL2 gcc 13.3, pinned core 4 (`taskset -c 4`), f64,
> single thread both sides, best-of-5, ms. Harnesses (tracked): our side
> `external/crd_ours_tensor_bench.cpp` (compiled against the WSL build-tree libs — the
> external-oracle precedent); peers `external/tblis/crd_tblis_bench.cpp`,
> `external/xtensor/crd_xtensor_bench.cpp`.
>
> **TBLIS** master (MatthewsResearchGroup/tblis, cloned 2026-07-05), CMake Release,
> `ENABLE_THREAD_MODEL=none`, BLIS haswell config built by its superbuild.
> **xtensor** master + xtl master (header-only), `-O3 -march=native`, `xt::noalias`
> assignments (their fastest documented path).

## Contraction vs TBLIS

| case | ours ms | TBLIS ms | ratio | verdict |
|---|---|---|---|---|
| `abc,bad->dc` @96 (transpose-heavy TTGT) | **3.496** | 3.974 | **1.14×** | **WIN** |
| `ab,bc->ac` @512 (pure GEMM shape) | 3.895 | 3.703 | 0.95× | **OPEN — the v0d f64 GEMM-kernel gap** |
| `ab,bc->ac` @1024 (pure GEMM shape) | 29.990 | 29.260 | 0.98× | ~tie, same named gap |
| `abcd,dcef->abef` @24 (GEMM-bound 4D) | 5.394 | 5.246 | 0.97× | ~tie, same named gap |

The split is diagnostic and matches the v14-f board exactly: where the TENSOR layer does real
work (permute/pack/unpack around the kernel — `abc,bad->dc`), we beat TBLIS outright; the
pure-GEMM-shaped rows reduce to raw microkernel throughput, where TBLIS *is* BLIS's f64
microkernel and the 2–5% deficit is the **already-open v0d raw f64 GEMM gap** (the v14-f
open cells, the GEMM-asm-reopen decision, proposed ADR-0100 fast-order tier). These rows are
that SAME open bug counted at three more cells — not a new einsum-layer loss; no new owner.

## Elementwise vs xtensor

| case | ours ms | xtensor ms | ratio | verdict |
|---|---|---|---|---|
| contiguous add 16M | **14.939** | 16.055 | **1.07×** | WIN (both at the DRAM wall) |
| broadcast add [512,1,512]+[1,512,512] | **71.564** | 241.864 | **3.38×** | **WIN** |
| strided mul (transposed B) 4096² | **181.168** | 214.355 | **1.18×** | WIN |

⇒ **xtensor: 3/3 WINS.** Its lazy-expression engine collapses on broadcast/strided access
(3.4× on broadcast); the contiguous row is bandwidth-saturated for both (the v14-b
DRAM-ceiling caveat applies).

## Verdict line

**TBLIS measured at last: the TTGT layer BEATS TBLIS where tensor-layer work exists (1.14×);
the three pure-GEMM rows (0.95–0.98×) are the pre-existing named v0d f64 GEMM-kernel gap —
the open bug's cell count grows, its root cause does not. xtensor: 3/3 wins.** With these,
every peer contracted anywhere in v14 (ADR-0096 §3 + the slice tables) has measured rows.
