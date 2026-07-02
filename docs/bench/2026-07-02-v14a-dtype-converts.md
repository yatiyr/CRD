# 2026-07-02 — v14-a dtype converts: f16/bf16/FP8 + deterministic SR vs numpy/ml_dtypes/torch

- **Machine/config:** i9-14900K, WSL2 Ubuntu 24.04, single pinned core (`taskset -c 4`), 1M elements,
  20 reps. Cerid: g++ 13.3 `-O3 -march=native -DNDEBUG` (F16C/AVX2 active). Peers single-threaded
  (`OMP_NUM_THREADS=1`): numpy 2.4.6, ml_dtypes 0.5.4, torch 2.12.0+cpu.
- **Harness:** `scripts/run_dtype_bench.sh` (compiles `scripts/bench_dtypes.cpp`, runs
  `scripts/bench_dtypes_peers.py`). Correctness gates: 16 cases / 103,496 asserts
  (`tests/hesap-tensor/`), bit-exact vs the ml_dtypes corpus + ggml `quantize_row_*_ref`.

## The board (ns/element; lower is better)

| Op | Cerid (batch) | numpy | ml_dtypes | torch | Verdict |
|---|---|---|---|---|---|
| f32→f16 | **0.099** | 1.217 | — | 0.146 | **1.5× torch-F16C · 12.3× numpy** |
| f16→f32 | **0.103** | 0.659 | — | — | **6.4× numpy** |
| f32→bf16 | **0.120** | — | 0.362 | 0.491 | **3.0× ml_dtypes · 4.1× torch** |
| f32→fp8 e4m3fn | **0.596** | — | 1.356 | — | **2.3× ml_dtypes** |
| f32→fp8 e5m2 | **0.604** | — | (e4m3-class) | — | ~2.2× |
| f32→f16 **SR** (deterministic) | **1.204** (scalar 7.113) | n/a | n/a | n/a | **5.9× own-scalar; cheaper than numpy's plain RNE convert. NO peer ships deterministic SR (checked: numpy/ml_dtypes none; torch no CPU SR)** |
| f32→bf16 **SR** | **0.860** | n/a | n/a | n/a | — |
| f32→e4m3 **SR** | **1.193** | n/a | n/a | n/a | — |
| quantize Q8_0 | 1.553 | — | — | — | ggml-native peer bench lands at v14-m (parity is byte-exact-gated now) |
| dequantize Q8_0 | 0.092 | — | — | — | — |

## Levers (recorded for reuse)

- f16 both directions: F16C `VCVTPS2PH/VCVTPH2PS` 8-wide (hardware IEEE RNE; scalar NaN semantics aligned
  to the hardware — payload truncate+quiet both directions — so SIMD ≡ scalar is bit-gated on ALL inputs).
- fp8: a generic 8-wide AVX2 integer transcription of the scalar `narrow_rne` (masks/blends/`vpsrlvd`).
- SR: 32 Philox draws per call via the hesap-stats AVX2 8-block kernel (lane-packed keying
  `block=idx>>2, lane=idx&3`, pinned) + `narrow8_sr_avx2`; deep-underflow lanes scalar-patched.
- `-mf16c` added to `crd-simd-flags` (GCC/Clang need it explicitly; MSVC /arch:AVX2 implies it).
