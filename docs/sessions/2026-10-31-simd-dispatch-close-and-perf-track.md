# 2026-10-31 — SIMD-DISPATCH closed by user order; its performance items move to SIMD-PERF

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

## User order

The user, 2026-10-31 about 15:40, after the row's boards had turned into eight hours of per-finding tuning against
the order of 2026-10-29 ("DO NOT GRIND"): "close the ... row NOW! CLOSE THE DAMN THING! USE ANOTHER VM AND WORK FOR
OPTIMIZATIONS ON ANOTHER MACHINE AND CARRY ON IMPLEMENTING THIS!"

## What changed

- [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) is Done by this order, not by its board acceptance. The
  contract, detection, kernel units and every family with its forced-level CI arms are in. Its open performance items
  moved word for word to the new row [SIMD-PERF](../ROADMAP.md#slice-simd-perf).
- Open at the close, against the pre-row engine or a peer:
  - the cnn rows trail pre-row by 11% to 16% at avx2;
  - MKL leads fft's f32 forward by 8% to 32% from 512K to 4M;
  - the Student-t sum's log costs 0.761x of pre-row, accepted under the 2026-10-30 rule.

  Against the peers at avx2 the boards stand as recorded:
  - SELL leads Eigen (geometric means 1.21 f64 and 1.45 f32);
  - permute leads HPTT but for two cells at 0.97 and 0.99;
  - GEMM is level with OpenBLAS (0.94x to 1.08x);
  - the statistics gradients lead JAX and PyTorch;
  - the gamma batches lead scipy and MATLAB;
  - the mlp leads onnxruntime and torch;
  - torch leads the nn activations by 5.9x to 20x on its f32 approximations.
- SIMD-PERF runs on a dedicated cloud benchmark machine in parallel with the serial roadmap, never on the development
  host. Every board there re-baselines the pre-row engine `2f2aed72` and HEAD on that machine, interleaved, judged
  by ratios; its numbers are never compared with the i9-14900K boards. A change reaches `main` only on a measured win
  under the 2026-10-30 acceptance rule.
- [HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune) follows the retune's boards into SIMD-PERF.
- CI-1, CI-2 and CI-3 were Blocked on the user's Part A of the setup recipe. Part A and the operator's Part B are
  done ([session](2026-10-31-ci-launcher-and-merges.md)), so each is Partial, naming the live evidence it still needs.
  CI-4 stays Open: it needs the parity proof and the user's go. The CI rows are worked by the CI track, beside the
  serial roadmap.
- The serial roadmap resumes at [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1), the context pointer by this
  order. SIMD-PERF, HESAP-JOBS-RETUNE and CI-1 to CI-4 sit before it and belong to their own tracks.
