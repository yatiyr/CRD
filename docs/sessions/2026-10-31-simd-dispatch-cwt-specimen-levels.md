# 2026-10-31 — SIMD-DISPATCH: the CWT fatal specimen under the per-level fft plans

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch) (ADR-0134 decision 7, the per-level fft plans). Before:
[the hosted short temp path](2026-10-30-simd-dispatch-ci-short-temp-paths.md).

## CI first

No completed run has a failed job: the cancelled push runs 41126260668, 41120249137 and 41118715232 hold none. Push
run 41112483075 (`37ebb440`, in progress, not cancelled) has four failed jobs:

- `Repository checks`, `windows-clang-cl-shipping` and `win-arm64-debug`: the `RUNNER~1` short temp path in
  `crd-ci-platform`, already fixed on main (previous note).
- `linux-gcc-debug-scalar`, new: `crd-hesap-wavelet-fatal-cwt-plan-refused` printed `CRD_DIAG_FATAL_OTHER` with
  `FftPlan: out of memory for its tables`.

## The cause

The specimen refuses exactly the request shape of an `FftPlan<f64>`, so that the transform's first per-job scale plan
is refused. Before the per-job plans, `cwt` builds two local plans: `central_frequency`'s of 2^precision points (4096
at the default precision of 12) and the data plan of 256. Since decision C's level table (`level_plans_gen.hpp`),
the portable and sse2 levels plan f64 4096 as a four-step, whose two sub-plans are heap objects of that same shape;
the first one was refused and `FftPlan`'s own fatal fired. avx2 plans 4096 as IP4-AoS, so win-debug passed. The
scalar lanes run portable; the win-debug lane's sse2 arm reruns the specimen and would fail the same way.

Reproduced on win-debug with `CRD_CPU_LEVEL`: portable and sse2 printed the other fatal, avx2 the expected one. Both
fatals are the engine's correct behaviour; the specimen's setup was level-dependent.

## The fix

The specimen passes a precision of 9, so `central_frequency`'s plan is 512 points and the data plan 256. Below 1024
points only Stockham builds (`forward_builds`), on every level and in every mode, so no setup plan has a sub-plan. A
`static_assert` holds that precondition, so a planner change that breaks it fails the build. The fatal under test is
unchanged. After the fix, portable, sse2, sse4.2 and avx2 each print `CRD_DIAG_FATAL_OK cwt-plan-refused`, and the CTest
passes on win-debug.

The other shape-refusing fft and dsp specimens were checked against the level table. None builds a request of its
refused shape before the one it names, on any level. In fft `plan-ip4-table-refused` and
`plan-four-step-subplan-refused`, the planned inverse keeps the named path where the level's forward differs.

Needs CI: DIAG.11c and DIAG.12 still wait on a green complete-tier run; none was dispatched (push runs pending).
Repository variables `FTL_GCP_PROJECT`, `FTL_WIF_PROVIDER` and `FTL_SERVICE_ACCOUNT` exist (names only). There are no
secrets, so the AMD and Mac lanes wait on their accounts.

## The SELL board

`build-bench.py host` refused the host: other processes 8.1 cores together, dota2 4.2 cores, Studio One 1.4 cores;
21.0 of 63.7 GiB available. The board did not run.

## Left

The SELL board's five passes on a quiet host, then permute, the sse2 bodies and the retune's thread sweep.
