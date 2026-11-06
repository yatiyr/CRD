# 2026-11-07: NET.2 closes on its hosted evidence; the Reed-Solomon code against OpenFEC

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [NET.2](../ROADMAP.md#slice-net.2) (Done), [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 100.

## NET.2: the audit of nightly 42202454758 (complete tier, 33 jobs, at 0294cdef)

- It contains every NET.2 commit. Every job's log was read for the crypto CTests and for `(Failed)`.
- P3: win-arm64-debug's configure prints `[crd] AWS-LC's ARM64 assembly: ...clang.exe --target=aarch64-p...`. The crypto
  CTests pass on all five ARM64 lanes (3,185, 3,182, 3,182, 3,179 and 3,179 tests in the lanes).
- Two jobs not green, neither through crypto:
  - linux-clang-arm64-asan: one failure, the guard `crd-check-provider-symbols` misreading `__odr_asan_gen_` (9532a6c8);
  - win-debug: all 9,897 tests pass, then the 300-minute cap inside the sse2 arm (474a37f8). Neither fix has a completed
    hosted run yet; 42282917539 at 474a37f8 is running (13 of 32 jobs done, none failed, when this was written).
- linux-gcc-asan's mikktspace failure in the same run is the registered TP-5.
- The row closes through the user-order override, as NET.1 does.

## OpenFEC

- Pin `openfec`: openfec.org is a parked domain with no archive (checked 2026-11-06), so the pin takes the maintained fork's
  tag v1.4.2.12 (roc-streaming/openfec, commit e549f3b8), which the note and provenance say. CeCILL-C, test only.
- **Finding:** OpenFEC does not build RFC 5510 §8.2's matrix. Its first row is (1, 0, ..., 0), so it evaluates at
  {0, alpha^0, alpha^1, ...}; §8.2 evaluates at {alpha^0, alpha^1, ...}. At k = 2, n = 3 the repair row is (3, 2) against
  the RFC's (2, 3). Its fast Vandermonde inversion needs that zero point (found when it failed on the RFC's matrix).
  hesap keeps the RFC's matrix, also held to ISA-L's arithmetic; no consumer asks for OpenFEC's wire form.
- `crd-hesap-erasure-openfec-tests` (5 cases, linux-gcc-public-checks):
  - every product, inverse and power against OpenFEC's tables;
  - §8.2's GM from OpenFEC's Gauss-Jordan inversion and multiply, for 10 shapes up to k = 254;
  - hesap's recovered sources against OpenFEC's inverse of the received columns (6 patterns per shape);
  - the real encoder against the Lagrange form of OpenFEC's points computed with hesap's field, and the real decoder
    recovering from those symbols, so the difference is the points and not a misuse;
  - the (3, 2) against (2, 3) case.

## Finding for the row: the NEON bodies have no hosted proof

No ARM64 lane builds hesap-comms: its closure reaches crd-jobs (hesap-dense, hesap-special, hesap-fft, hesap-quadrature), and
`CRD_MODULES_WITHOUT=jobs` drops it. Batches 94 and 97 asked for a hosted NEON check that cannot happen. A clang syntax
check of `erasure_kernels.cpp` at the NEON level for aarch64 passes; nothing has run. `crd-net` is jobs-free today and would
lose its ARM64 lanes when it links these modules, so the split of the jobs-dependent parts comes before NET.3 links them.

## Next

Audit 42282917539 (win-debug under the cap, provider symbols); the closure split with the NEON arm on ARM64 lanes; the
estimators with a libwebrtc oracle (pinned file by file from webrtc.googlesource.com at d2a33df5).
