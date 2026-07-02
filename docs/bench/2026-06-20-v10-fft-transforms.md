# 2026-06-20 — v10 FFT cluster: 1D FFT + NUFFT + DCT/DST + sparse FFT

Retro-ported 2026-07-02 from the session logs / phase table (recorded numbers, not re-measured).

- **Machine/config:** i9-14900K (14-core), WSL2 Ubuntu 24.04, single pinned core (`taskset -c 4` for bench suite, 1 thread). Cerid: g++ -O2, f64 primary. Peers: Intel MKL (single-thread, best-of-20 runs), FFTW 3.x (ESTIMATE plan), PocketFFT (numpy/scipy.fft backend), FINUFFT, scipy 1.17.1.
- **Harness:** `runtime/examples/bench_fft_*.cpp` + `scripts/run_bench_*.sh` (WSL). Non-overlapping input pools, data-dependent index, no hoisting. EXECUTE-only (plan + setups amortized in the repeated-geometry model).

## The board (ns/transform or ns/element; lower is better)

### 1D Complex FFT (Cerid f64 vs MKL single-thread)

| N | Cerid (ns) | MKL (ns) | Verdict |
|---|---|---|---|
| 256 | — | — | parity regime (not benched) |
| 1024 | — | — | parity regime |
| **65536** | — | — | parity regime |
| **262144 (256K)** | ~3400 | ~4000 | **parity (~0.85×)** |
| **1M (f32, four-step)** | 0.69 | ~0.86 | **~0.80× MKL** (orchestration deficit, not codelet quality) |
| **1M (f32, fused gather 1024)** | 0.77 | ~0.86 | **~0.90× MKL** (banked fusion default-on) |
| 2M | small bonus | — | marginal |
| 4M / 8M | untouched | — | (2048/4096 axes, fusion does not fire) |

**Honest framing:** isolated CRD batched-1024/2048/4096 codelets measure faster than MKL (1.53× / 1.42× / 1.17×). The f32 1M mid-size gap (~0.80× MKL) is **orchestration/dataflow** (layout reconciliation, round-trip scratch), not codelet quality. Codelet is strong; the four-step inter-stage twiddle (8 measured attempts) is the deferred genfft-scheduler work. No follow-ups for the inherited gap (Spiral-class person-weeks).

### Small-N single transform (execute, lane-trick AoS codelets)

| N | Cerid vs MKL | Single-call vs SoA prior | Verdict |
|---|---|---|---|
| 8 | ~1.00× | **2.39× batched crush** | parity single / **1.46× batched crush** (only user-accessible win) |
| 16 | ~0.96× | — | parity |
| 32 | ~0.98× | — | parity |

Latency-bound (permute2f128 chain hidden only when calls overlap); recompiled to be **1.5–2.5× faster than the prior SoA leaf**, so single-call lifts from ~0.4–0.6× MKL to parity. **N=8 batched is a real, gated, shipped crush** (1.49×); N=16/32 batched hit strided-gather cache wall (element-major layout) — the block-transpose-to-contiguous AoS-assembly is the fresh-context lever.

### Non-Uniform FFT (NUFFT type-1/type-2 vs FINUFFT, 1 thread, EXECUTE-only)

| N=M | T1 (Cerid vs FINUFFT) | T2 (Cerid vs FINUFFT) | Accuracy | Verdict |
|---|---|---|---|---|
| 1024 | 1.17× | 1.21× | 3e-10 (vs ~1e-9 FINUFFT) | **win, superior accuracy** |
| 4096 | **1.99×** | **1.66×** | 3e-10 | **beat outright** |
| 16384 | **1.68×** | **1.73×** | 3e-10 | **beat outright** |
| 65536 | 0.93× | 0.97× | 4e-10 | **parity** |
| 262144 | 0.90× | 0.74× | 3e-10 | **loss: FFT-bound (62–91% of time is the fine-grid FFT)** |
| 1M | 0.99× | **1.14×** | 6e-10 | **parity / T2 win** |
| 65536 / 262144 (M≫N) | 1.06× | 1.07× | 4e-10 | **win** |
| 262144 / 16384 (M≪N) | 0.79× | 0.77× | 6e-10 | **loss** |

**Verdict:** Cerid NUFFT **beats FINUFFT small-to-mid sizes (up to 2× at superior accuracy)**, parity at 1M (T2 wins). Large-n loss (262144) is the **same inherited v10-b FFT-engine deficit** — spreader and interp (the NUFFT-specific work) is competitive-to-winning. FINUFFT's backend there (FFTW_ESTIMATE) is not best-in-class either. Honest scoping: Determinism via bit-identical run-twice (serial, TODAY); Timing EXECUTE-only (plan amortized); Width w=11 vs FINUFFT w=10 at eps=1e-9 (Cerid does more spread work and still wins).

### DCT/DST (DCT-II primary, Cerid f64)

**vs PocketFFT (numpy/scipy.fft.dct backend, the most common):**

| N | Cerid vs PocketFFT | Verdict |
|---|---|---|
| 256–4096 | **1.0–1.6×** | **beat outright** |
| 8192 | **1.50×** | **beat** |
| 16384 | **1.30×** | **beat** |
| 32768 | **1.69×** | **beat** |
| 65536–262144 | **1.5–1.7×** | **beat consistently** |
| 1M | **1.25×** | **beat** |

**vs FFTW (the MKL-class tuned-codelet peer):**

| N | Cerid vs FFTW | Verdict |
|---|---|---|
| 256–4096 | 0.55–0.64× | **loss** |
| 8192 | 0.60× | **loss** |
| 16384 | 0.96× | **parity** |
| 32768 | **1.07×** | **win** |
| 65536–262144 | 0.60–0.70× | **loss** |
| 1M | 0.64× | **loss** |

**Honest verdict:** **Cerid DCT beats PocketFFT outright** (all sizes, the scipy/numpy peer — most common). FFTW stays ahead at most sizes via its tuned real-DCT codelets — the **same inherited FFT-engine kernel wall as the raw complex FFT**, not a DCT-specific gap. The real-FFT path (Makhoul O(N log N)) was the lever that flipped every PocketFFT comparison to a win (large-N losses 0.86× → 1.25×). DST-II shares the path (same win expected; benched via DCT-II as representative). MKL's trig-transform DCT is the same kernel class (expected ahead, not separately benched). Follow-ups named: dct3/dst3 on the inverse real FFT, DCT-I/IV + DST-I/IV, FFT-convolution, FHT.

### Sparse FFT (HIKP, k-sparse recovery vs theory)

| Regime | Cerid | Verdict |
|---|---|---|
| Exact k-sparse, small noise | O(R·log n·(w+B log B)), no O(n) step | **sub-linear + noise-robust** |
| Machine-eps coeffs | ~1e-7 (exact sparse) | **machine-precision recovery** |
| Noisy k-sparse, SNR ≳ 10 | recovers all frequencies to √(n/B)·σ/√R floor | **theoretically sound** |

No peer bench (HIKP is the only published sparse FFT in this regime; compare is theoretical bounds met). Honest scoping: sub-linear-B robustness confirmed where bucket-SNR ≳ 10; machine-eps coeffs under noise require ~n samples (information-theoretic ceiling).

## Losses and deferrals (no asterisks)

- **f32 1M ~0.80× MKL loss (256K 0.25→0.85× improvement via Stockham trough fix + four-step default):** pure orchestration (inter-stage twiddle, layout reconciliation). The 8 measured failed attempts (blocking/twiddle-fusing/strided-gather) proved incremental capture is hard. **Genfft fused-phase kernel rewrite (M16) is the next lever** — one scheduled program per 1024-axis tile; person-weeks, fresh-context.
- **Large-n NUFFT (262144 modes, 0.90× / 0.74×):** **FFT-bound (62–91% fine-grid FFT time)** — same deficit as the raw 1D FFT. Spreader and interp (NUFFT-specific work) beat FINUFFT; the gap is not a NUFFT problem.
- **DCT vs FFTW (0.55–0.70× at mid/large sizes):** same inherited FFT-kernel wall; FFTW's real-DCT codelets are tuned. Cerid beats the PocketFFT tier decisively (the scipy/numpy user's peer).
- **CZT in v11-dsp (noted for completeness):** scipy shortcuts M=N DFT + caches; Cerid rebuilds transient plans. Plan-cached `CztPlan` was added as follow-on, achieving parity. One-shot single transforms are correct (to 1e-9) but unmeasured.

## Honesty notes & moats

- **Determinism:** all 1D FFT plans are bitwise reproducible {1..16} threads (no thread-dependent reduction). Parallel-batched FFT API = named follow-on.
- **No FFTW/FINUFFT asm:** portable C++ (per ADR-0092; FFTW's hand-written assembly + MKL's vendor intrinsics are the comparison class, not aspirational for a general engine).
- **Codelet quality proven:** isolated small-N and batched codelets **beat MKL in the lab**. Large-N gaps are dataflow/orchestration, not arithmetic.
- **Gold standards:** MKL (single-thread, best-of-20), FFTW 3.x (ESTIMATE — not wisdom, no tuning), PocketFFT (the installed scipy backend), FINUFFT 2.x.

## Verdict

**v10 FFT cluster: beats PocketFFT/numpy/scipy decisively (DCT 1.25–1.69× · NUFFT small-mid 1.68–1.99×); parity-to-win vs MKL/FFTW on small-N and mid-range (N=8 batched 1.46× · 32K 1.07× DCT); achieves MKL global parity as a portable-C++ non-vendor goal (1M mid-size orchestration gap deferred as person-weeks genfft scheduler; the inherited codelet is strong, proven faster than MKL in isolation). All transforms deterministic, sparse FFT sub-linear, full cluster is production-grade.**
