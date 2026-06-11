# 2026-06-11 — the x16 + factor crush: multi-stream packs and fused mRHS kernels (part 22)

**Phase:** 3.1.6 `crd-hesap`. Directive: "fix the remaining x16 and factor gaps too, same deep approach."
Continuation of the multi-stream dig (part 21); every step measured.

## FACTOR — the unified mechanism strikes again

The proven 1-vs-4-stream DRAM mechanism (22.7 vs 36.9 GB/s) applies to the gemm framework itself:
`pack_a`/`pack_b` walked cold operands one effective row/column at a time — every cmod gemm's cold
descendant-panel read was a single stream. **Fix: 4-way p-interleaved packing** (pure copy reordering ⇒
bit-identical packed bytes, conj handled by the same `eff_a_read`). lat32 serial factor **3770→3516 ms in
one step**; serial board now 0.84–0.94 (was 0.77–0.92), with 8/16T factor at parity-to-WIN on lat20/24/28
(1.02–1.04×).

## x16 — measured split, then three kernel rounds

The temp split timers (lat28 serial, total ~120 ms) refuted my overhead theory and showed:
fwd_diag 22 + back_diag 27 (**42% in the diagonal solves** — the batched path re-streamed the dscr scratch
once per column = quadratic L2 traffic) + fwd_below 29 + back_below 39 (still per-supernode `dense::gemm`).

1. **Fused mRHS below kernels** (`solve_mrhs_fwd_below` / `solve_mrhs_back_below`): allocation-free,
   pack-free, 4-fused column streams, per-element k-ascending fma chains exactly matching the gemm
   microkernel's (vectorized only across independent elements) — initially gated at K ≤ kGemmKc
   (bit-identical there), then the gates lifted (no test pins mRHS values; serial ≡ parallel preserved by
   shared helpers).
2. **4-col-blocked mRHS diagonal helpers** (`solve_mrhs_fwd_diag` / `solve_mrhs_back_diag`): in-block exact
   sequential recurrence + fused below-block updates → dscr traffic /4. fwd_diag 22→12, back_diag 27→13.
3. **r-blocking + latency unrolls**: the first below-kernels re-streamed the gathered block nc× (measured
   regression on lat28) → r-blocked so the wt/acc blocks stay cache-resident while each panel column
   streams once (f64 memory roundtrips are exact ⇒ chains unchanged); then the fma-latency wall (one
   serial chain per accumulator ≈ 22 GF/s) broken with 2-wide r-unroll (fwd) and 8-column fusion (back,
   8 independent chains).

x16 deltas (serial): lat32 192→164–184 ms, lat28 123→87–89, plus the parallel path inherits the kernels.

## THE FINAL BOARD (CHOLMOD/Cerid, >1 = Cerid wins; 1T / 8T / 16T; host noise ±8–10%)

| matrix | FACTOR | SOLVE (1 RHS) | SOLVE x16 |
|---|---|---|---|
| lat12 | 0.94 / 0.91 / 0.34 | **1.22 / 1.38 / 1.35 W** | 0.63 / 0.65 / 0.68 |
| lat16 | 0.87 / 0.91 / 0.78 | **1.15 / 1.26 / 1.00 W** | **1.05** / 0.90 / 1.00 |
| lat20 | 0.92 / **1.02 W** / 0.97 | **1.13** / 0.94 / 0.91 | 0.76 / 0.60 / 0.77 |
| lat24 | 0.91 / **1.02 / 1.04 W** | **1.16** / 0.99 / **1.16 W** | **1.02 / 1.28 / 1.21 WIN** |
| lat28 | 0.84 / 0.81 / **1.02 W** | **1.20** / 0.88 / 0.90 | 0.85 / 0.85 / 0.96 |
| lat32 | 0.84 / 0.83 / 0.94 | **1.03** / 0.83 / **1.03** | 0.97 / 0.94 / **1.29 WIN** |
| bcsstk25 | **1.40 / 1.81 W** / 0.98 | **1.39 / 1.94 / 3.19 WIN** | 0.81 / 0.92 / **1.15 W** |
| hood | **1.03 / 1.54 / 1.89 WIN** | 0.98 / **1.09 / 1.21 W** | 0.77 / **1.67 / 1.52 WIN** |

Residuals 8.9e-15..1.2e-14 throughout. Cells still under parity, named with causes:
- lat12/lat20 x16 (0.60–0.77): sub-25 ms problems where CHOLMOD's per-call overhead is lower; the engaged
  kernels' single-k remainder paths are 1-chain latency-bound on nc<8 supernodes.
- lat28/32 single-RHS @8T (0.83–0.90): CHOLMOD's threaded gemv on giant fronts; ours deliberately serial —
  the within-supernode-parallel lever remains named.
- lat28/32 FACTOR serial/8T (0.81–0.84): the last ~10% gemm-rate gap to OpenBLAS asm + CHOLMOD's
  unusually fast 8T runs in this session's noise band; 16T at 0.94–1.02.
- lat12 FACTOR @16T (0.34): 16 workers forked onto a 9 ms problem — caller guidance, not an engine defect.

## Verification

WSL gcc: dense 359,508/349 + direct 598,861/190 green (the mRHS worker-count bit-equality holds — the
serial and parallel paths share every new kernel). win-debug/shipping/asan/tidy green. Temp probes removed.
