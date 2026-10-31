# 2026-10-31 — SIMD-DISPATCH: nn's activation spans in the nn unit

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (the sse2 boards' findings: nn's gelu, tanh and sigmoid
spans). Before: [the Student-t sum and the scalar table](2026-10-31-simd-dispatch-student-t-log.md). Evidence:
[sse2 boards, the activation spans](../bench/2026-10-31-simd-dispatch-sse2-boards.md#the-activation-spans).

## CI first

- No completed run has a failed job; push run 41167488972 (`d9fdeae3`) passed. Scheduled run 41188120176 is in
  progress on GitHub's copy of main, still `d9fdeae3`: the GitLab push mirror has carried nothing since, so no later
  commit has a hosted run. Nothing was cancelled or dispatched.
- Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run. Repository variables `FTL_GCP_PROJECT`,
  `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only), no secrets; the Firebase lane is wired in `ci/`,
  the AMD and Mac lanes wait on their accounts.

## The move

- `tensor_kernels.hpp`: `NnAct` and `NnActF32` (samples `[lo, hi)` of `per` values) and the table entry
  `nn_act_rows`; `tensor_nn_kernels.cpp` defines it per level: relu as the Max walk against +0, gelu, tanh and
  sigmoid as the scalar formulas, so the f64 exp and tanh bodies are the unit's own (the level's fma or its exact
  emulation). The stand-alone `nn_relu` keeps `relu_span`.
- `nn.hpp` lost `gelu_one`, `sigmoid_one`, `tanh_one`, its `NnAct`, `EwCtx`, `ew_run` and its
  `crd/math/deterministic.hpp` include; the spans, the activation ops and the fused linear relu of `NnSequential`
  call `nn_act_rows`.
- The premise held for two of three: sigmoid's `exp` and tanh were scalar-table calls in general code; gelu's
  `deterministic::erf` is Cephes in general code without fma, the same call before and after.

## Checks

- `crd-hesap-tensor-tests`: a new `[kernels]` case runs every level the host runs against the formulas in general
  code (relu, gelu, tanh, sigmoid; widths 1, 3, 8, 37; rows `[1, 4)` between guard words; in place and not; NaN,
  infinities, signed zeros, subnormals, saturating and underflowing arguments; NaN payloads aside, ADR-0134 §7).
  The 240 tensor-kernel and nn cases pass on win-debug and linux-gcc-release at the detected and every forced level;
  the case passes under SDE (`-spr`) on all six tables, avx512 and its VNNI variant included.
- The 19 repository guards (`crd-check-isa-isolation` among them), strict tidy on the changed files (bench_nn on the
  Linux tree, where retained benches build), clang-format and Allman.
- `bench_nn`'s new `act` rows and the board:
  [record](../bench/2026-10-31-simd-dispatch-sse2-boards.md#the-activation-spans). The move changed nothing beyond
  noise (before over after 0.99 to 1.03 at every level), so the item closes as a structural move, not a speed fix;
  torch leads the spans 5.9x to 20x, recorded under the 2026-10-30 rule.

## A local event

Under `ctest -j 8`, the permute region case failed twice in about 85 runs of the same binary, once at the detected
level and once in the forced avx2 arm (signature: `avx2+avx-vnni Complex<f32> na 16 nb 40 sa 1 sb 0 tiles 64x64
alpha -0.5+0.25i stream 1 next 1 lead 0`, 8 scalars off). The case is single-threaded, its inputs fixed and its
buffers fully initialised, and its stores are fenced; 60 six-wide reruns and three full sets passed. The TSan host
check (20 rounds) recorded 0 events, which the 2026-10-26 note says does not expose this host's fault; push run
41167488972 ran the case on every hosted lane and level, green. Recorded per the host lesson, not chased as code.

## Left

The row's Left list keeps the cnn rows' 11% to 16%, then the jobs-retune thread sweep and fft f32 against MKL.
