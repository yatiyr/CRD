# 2026-11-07: the network math gets one jobs-free module, crd-hesap-net

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 101.

## CI

Nothing to fix. 42282917539 (474a37f8): 15 of 32 jobs passed, none failed, android, macos and win-arm64-debug among them;
win-debug and linux-clang-arm64-asan still running. 42298265296 (7b1b1b32) is queued.

## Why

Every home the row named for the network math reaches crd-jobs, which has no ARM64 fiber context before JOBS-PORT.2:
`hesap-comms` (hesap-dsp, hesap-fft), `hesap-stats` (parallel resampling, hesap-dense, hesap-special), `hesap-interp`
(hesap-dense, hesap-fft). `crd-net` links this math and is jobs-free, so each remaining clause would hit the same wall, and
no hosted lane could ever build the NEON bodies. A network client should not link an FFT on any target either.

Decision: one module, named for the row, on crd-core and crd-containers only. The estimators, clock filters, dead-reckoning
and rewind kernels and quantisation helpers land there too ([ADR-0065 §29's amendment](../decisions/0065-hesap-numerical-substrate.md)).

## What landed

- `engine/numerics/hesap-net`, namespace `crd::hesap::net`: the Reed-Solomon code, FlexFEC, RED, the interleavers, GF(2^8) and
  the `hesap.erasure` kernels moved from `hesap-comms` by `git mv`. `numerics.erasure-codes` and the FEC register entries
  now name it; `hesap-comms` is modulation and OFDM again.
- `tests/numerics/hesap-net` with the fuzz targets, their corpora and the ISA-L and OpenFEC peers. CTest names stay
  `crd-fuzz-hesap-*` and `crd-hesap-erasure-*`; the kernel arms are `crd-hesap-net-kernels-at-<level>`.
- The tests took Philox from hesap-stats only as a byte source. They use crd-core's SplitMix64 (`seeded_words.hpp`), the
  `random.seed-generators` answer to the same reachability problem.

## Evidence

- win-debug, fresh configure: crd-hesap-net-tests 135,746 assertions in 35 cases; crd-hesap-comms-tests 39,663 in 27; the 27
  kernel arms up to avx512 and the 3 corpus replays; the 22 `crd-no-*`, `crd-check*` and diag guards.
- linux-gcc-public-checks: the same two suites, ISA-L every body 396 assertions, OpenFEC 941, the board's smoke run.
- The ARM64 selection on x64: linux-gcc-debug with `CRD_MODULES_WITHOUT=jobs,gpu-context-vulkan,platform-window` configures,
  builds and passes the module, with no `crd-jobs` in its build.ninja. The NEON arm itself waits for a hosted ARM64 run.

## Open

NET-DEBUG's seeded loss and delay models: `random.engines` (hesap-stats) owns the distributions, in the closure crd-net cannot
reach. A design question for that clause, not settled here.

## Next

Audit the ARM64 lanes for `crd-hesap-net-kernels-at-neon`; the estimators in hesap-net with the libwebrtc oracle.
