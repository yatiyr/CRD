# 2026-07-05 — the v14 parallel wave: i/j/k/l shipped concurrently (+ the sparse-CP glue)

> The user's directive: close v14 fast WITHOUT quality loss — implement independent slices
> in parallel, crush all peers, no deferrals. Method: four concurrently-run implementation
> agents, each owning ONLY its new files (headers/tests/oracles/gate scripts/bench doc),
> validating via direct-g++ gates against prebuilt engine libs (zero build-dir contention);
> the main session owned every shared file (CMake, docs), all Windows-ladder verification,
> integration glue, and serial board re-measurement on the merged artifact. Boards:
> `docs/bench/2026-07-05-v14{i,j,k,l}-*.md`. Same-day siblings: the v10 shipping close,
> v14-g, v14-h (own logs).

## What shipped (all uncommitted)

| Slice | Module files | Suite | Board |
|---|---|---|---|
| v14-i sparse | `sparse.hpp`, `sparse_mttkrp.hpp` | 890 asserts | **10/10 WINS**: MTTKRP **1.5× vs SPLATT v2 built from source**, 2.3× vs TACO (their broken `-time` CLI patched on THEIR side for honest numbers), 6–10× vs scipy/torch; TTM 2.7–2.8× vs TACO |
| v14-j decomp | `decomp.hpp` | 191 asserts | **9/9 WINS** vs TensorLy 0.9 at equal-or-better fit (CP 5.2–5.6×, HOOI 5.1–5.8×, rand-Tucker 1.2–1.7×) |
| v14-k TT | `tt.hpp` | 280 asserts | **8/8 WINS** vs tntorch (eval 13.1×, cross 12.6–15×); the 16⁶ LUT demo: **1748× compression from 19,008 evals, 1.50× faster than materialized-table interp** |
| v14-l I/O | `io.hpp`, `detail/io_zip.hpp`, `dlpack.hpp`, vendored `dlpack.h`, `hesap-resources/tensor_artifact.hpp` | 655 asserts | **12/12 WINS** vs numpy/safetensors-python (writes 1.8–3.0×, st-read to 6.3× / 11.15 GB/s); npy writer byte-identical to np.save 17/17 |
| glue (integrator) | `sparse_cp.hpp` + `test_sparse_cp.cpp` | 16 asserts | seam parity: CSF functor ≡ DenseMttkrp fits, identical iteration counts; run-twice bit-identical |

Full detail per slice → the phase rows (both homes) + each board doc.

## The method's quality evidence (why parallel did NOT cost the bar)

- Every agent ran reconstruct-verify-first (frozen oracles proved against the reference
  implementations BEFORE freezing) and the fresh-scars rulebook (no per-lane conditional
  multi-array loops; owned lifetimes; reserve-before-spans).
- **Rule #9 fired three times inside the wave and was executed, not recorded**: v14-j's
  first board lost EVERY row (root cause: 117 ms bidiagonal SVD per unfolding → Gram+
  `eig_sym` + AVX2-batched Philox sketch + Gram-operator power iteration → 9/9 flipped);
  v14-k's 32⁴ row was 0.53× (→ QR-first unfolding, flipped every tt_svd row); v14-i had
  three losing rows (→ register-resident fiber accumulators, staged TTM tile, branchless
  packed-key merges — an A/B probe proved branch misprediction was the loss).
- v14-i caught a REAL moat violation by inspection (TTM's serial path bypassed the
  fixed-count partial-buffer grouping) and fixed it pre-board.
- Boards were re-measured serially by the integrator on the merged artifact — headline
  rows reproduced (MTTKRP 10.4–10.6 ms, TT eval 54.6 ns/pt, CP 8.14 ms).

## Integration fixes (main session)

- MSVC hygiene the gcc-only agent gates could not see: `fopen` → `fopen_s` wrapper in
  io.hpp's CFile (the perf/capture.cpp precedent); scoped-pragma getenv dev knob in
  test_io.cpp.
- Two more pre-existing `std::sort` → 14.51-xutility tidy trips (`resources/src/crdr.cpp`)
  → `crd::containers::sort` (determinism-aligned; resources 78/78 re-verified).
- tidy findings in test_io.cpp: raw-string literals for the JSON adversaries (the
  `A` escape-parsing case kept its escape inside the raw literal), `zeros4` local
  per the lower-case local-constant convention.
- `SparseCsf` non-default-constructible → eager per-mode array init in the glue functor.

## Verification at close (per slice, the shipped artifact)

linux-gcc (direct-gate + suites) ✓ · win-debug ctest ✓ (io 14/14, tt 11/11, decomp 8/8,
sparse 12/12, resources 78/78) · win-asan zero errors ✓ (655/280/191/906) · win-shipping
(LTCG) ✓ all four · win-tidy ✓ all four. The whole-engine per-slice-check sweep is
deliberately deferred to the v14-z close (one sweep over the final artifact).

## Honest opens (tracked, not forgotten)

1. ~~MATLAB rows~~ **LANDED same evening** (service restored after being down all day —
   5201 × four checks): `scripts/v14_matlab_board.m` + `v14_matlab_ttb.m` (Sandia TTB
   cloned to `~/tools/tensor_toolbox`), R2026a, 1T, best-of-5. **ALL WON**: v14-h
   pagemtimes 2.0–8.2×, pagemldivide vs our LU factor+solve (a new `ours_lu_fs` harness
   row) 1.11–1.14×; v14-j TTB cp_als 1.67×/1.11× + tucker_als 1.63× — **with `'tol',0`
   forced: TTB's default tol early-stops (12.9 → 39.2 ms at 32⁴), a protocol trap now
   recorded in the board**; v14-i TTB sptensor mttkrp 29–39×. Every 2026-07-05 board is
   complete across ALL contracted peers.
2. **v14-m in flight** (agent working); **v14-z not started** (CLI, system doc, ADR-0096,
   all-peers scoreboard, conformance audit, whole-engine sweep).
3. Sparse-CP end-to-end perf row not benched (the dominating kernel IS crushed vs SPLATT).
4. Agent HOME→ flags for v14-z or later: thin-Q builder + rectangular LU → hesap-dense;
   dense rSVD's counter_gaussian → Philox migration candidate; TNSR ILoader registration;
   `TensorStatus::NotConverged` member when tensor.hpp next opens; inflate/CRC-32 as the
   seed of a future compression module; `-Wstringop-overflow` false-positive-looking
   report at `svd_dc.hpp:828` under gcc -O3 (pre-existing, flagged by the j-agent).

## The evening close (same day): v14-m + v14-z — v14 COMPLETE

- **v14-m landed + the last cell crushed**: NN inference pack (`nn.hpp`) — f32 board 8/8
  (torch 1.9–9.7×, ort 1.15–2.1× after profiled fixes: direct 3×3 conv, conv+relu+maxpool
  fusion), Q8_0 board vs torch-int8 6.6–10.2× + **ggml/llama.cpp built from source and
  beaten per layer at native ISA**, and the one open cell (ort-int8 MLP@4096, a
  per-tensor-vs-block-32 format difference) CLOSED the same evening with the **per-tensor
  i8 throughput tier: packed MLAS-structure kernel, 155.1 ns vs ort's 191.1 = 1.23× at
  better accuracy — EVERY CELL ON EVERY v14-m BOARD WON.** 18 cases/5,194 asserts;
  D-v14m-1 (corpus-quantizer divergence) documented in-header.
- **v14-z shipped**: the 12-command `hesap.tensor.*` CLI (ADR-0081 registry pattern,
  include-only link edges preserving the ADR-0096 link-isolation gate; `test_cli.cpp`
  5/5), `docs/systems/hesap-tensor.md`, ADR-0096 Amendments appended, and the
  **all-peers scoreboard** `docs/bench/2026-07-05-v14z-scoreboard.md` with the
  moat→ctest conformance audit. The consolidation's 10 cross-doc discrepancies were all
  fixed same-evening (stale verdict lines, two-protocol quote, missing e/f detail
  verdicts, the bench README index).
- **The two never-measured peers measured** (`docs/bench/2026-07-05-v14z-tblis-xtensor.md`):
  TBLIS — the TTGT-heavy case WON 1.14×, three pure-GEMM rows 0.95–0.98× = the
  already-named v0d f64 GEMM-kernel gap (same bug, three more cells, no new owner);
  xtensor — 3/3 wins (broadcast 3.38×). **Final scoreboard: 210 rows — 199 won, 7
  tie/parity, 4 open, and all four open cells are the ONE named v0d GEMM bug.**
- Module state: win-debug ctest **133/133 env-free** (CMake test-env properties; the
  em-dash ASCII scar fixed in the old v14-c moat test name); the whole-engine
  per-slice-check sweep is the final gate (running at log time; result recorded in
  context.md + the phase status line).

v14 a–z: every slice shipped, every contracted peer measured, every board row won or
pinned to the one named pre-existing kernel bug. Commit proposal → the user commits.
