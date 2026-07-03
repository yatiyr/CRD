# 2026-07-03 — v0d GEMM crush pass (order-preserving): ZeroInit + alpha==1 merge

- **Trigger:** the v14-f einsum table's GEMM-bound cells (chain @512 0.82–0.96×, `abc,bad` 0.89×).
- **Constraint honored:** ADR-0063 — Kc slab boundaries + the micro-tile-then-merge structure define
  every c_ij's rounding chain engine-wide. All changes below are BIT-IDENTICAL by construction and
  gated by the recorded-reference suites (dense 359,508 asserts + sparse + all tensor targets, green).

## What landed (measured on the standalone board, f64 1T)

| Lever | Result |
|---|---|
| **E3a ZeroInit kernels** — `gemm_packed_inner`/syrk zeroed a stack micro-tile, then the kernel LOADED those zeros; now a `ZeroInit` template starts the accumulators at 0 in registers (identical bits, no zero-store + no zero-load pass) | the bulk of the win |
| **E3b alpha==1 merge** — explicit branch skips the per-element multiply (IEEE `1·m == m` exactly) | small add |
| **Board: 65–69 → 70–77 GF/s (+8–12%)** vs OpenBLAS 79–81 / MKL ~78 | gap halved: 0.82–0.85× → 0.88–0.96× |

## Refuted by measurement (reverted; recorded so nobody retries them blind)

1. **E1 k-unroll ×2**: 4 live B registers + 12 accumulators spills past 16 YMM — REGRESSED to 60–66.
2. **E2 A-interleaved packing** (BLIS p-major): the strided pack writes cost more than the
   one-line-per-iter kernel reads gain — REGRESSED vs E3-alone (69–75 vs 70–77).
3. **Mc sweep (240/360/480)**: order-free (only Kc is bit-relevant) but flat within noise.

## The einsum table after the pass (matched-state A/B, 2 stable rounds)

| Case | Cerid | numpy | torch |
|---|---|---|---|
| plan-reuse matmul | **1.84 µs** | **4.0×** | **3.7×** |
| TN network @24 | **1598–1621** | **1.46–1.48×** | 0.97–0.99× (parity) |
| `abc,bad->dc` @96 | 3512 | 0.89× | **1.27×** |
| chain @512 | 8117–8121 | 0.97× | 0.86× |

## The terminal boundary (a DECISION, not a deferral)

The remaining 3–14% on the GEMM-bound cells is now precisely the cost of the bit-locked two-pass
accumulation (micro-tile → memory merge, per Kc slab) that MKL/OpenBLAS's beta-fused single-pass
kernels don't pay, plus their asm scheduling. Closing it requires the **ADR'd opt-in fast-order GEMM
tier** (beta-fused accumulation directly into C — still run-to-run and {1..16} deterministic, but
DIFFERENT bits from the legacy path). That is a deliberate engine-semantics decision (einsum results
would differ from hesap-dense gemm results unless both switch): proposed as ADR-0100 for the user's
call, with the einsum executor as the first opt-in consumer.

## Increments E4 + E5 (2026-07-03, same day): the fused-merge pass — OpenBLAS-class

**E4 fused-merge kernels** (`gemm_microkernel_avx2_{f64,f32}_fused`): the packed-inner pipeline
stored the finished tile to a stack buffer, then a merge loop re-read it (`c += alpha·micro`). For
FULL RowMajor tiles the fused kernel now updates C directly from the accumulator REGISTERS with the
exact same elementwise op sequence (vector mul-then-add mirrors the scalar two-rounding merge;
alpha==1 = a single add) — bit-identical, ~96 memory ops per tile call gone. Edges/ColMajor keep the
micro+merge path. **E5 BLIS-style C prefetch** at fused-kernel entry (the merge otherwise takes cold
C misses after the whole k-loop; order-free hint).

| Increment | f64 board (1T, GF/s) |
|---|---|
| baseline | 65–69 |
| E3 (ZeroInit + alpha==1 merge) | 70–77 |
| E4 (fused merge) | 73–78 |
| **E5 (C prefetch)** | **75–81** |
| OpenBLAS same-day | 80–84 (its own ±3% daily swing) |

**Cumulative: +15–18%, bit-identical throughout** — dense 359,508-assert recorded-ref gate green
after every increment (+ sparse + all tensor suites + asan + strict tidy). The remaining ~4–7% at
some sizes is inside this box's daily variance at n≤512 and a genuine small residual at 768–2048;
the ADR-0100 opt-in fast-order tier remains the sanctioned path past it if ever needed.

**einsum table impact:** the TN cell FLIPPED to a win vs torch (1.00–1.04×; 1.47–1.54× numpy);
chain = 0.94–0.97× numpy / 0.84–0.86× torch with the overhead decomposition now measured (our
einsum machinery overhead ≈ numpy's ~1.2 ms; the cell difference is purely the residual dgemm rate).
