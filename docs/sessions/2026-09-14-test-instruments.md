# Test instruments: bounded ingestion fuzzing, the sanitizer model of the fibers, ThreadSanitizer's verdict

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.9](../ROADMAP.md#slice-repo.dev.9); contract:
> [test instruments](../design/test-instruments.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [public consumption](2026-09-14-public-consumption.md).

## User direction

The standing direction is unchanged: carry the REPO.DEV slices on "one by one until AUD-2". REPO.DEV.9 followed
REPO.DEV.8 in the same session; the advisor ruled at the design gate (three deliverables, three proofs, no new
hosted lane; a replay executable per target on every lane and libFuzzer under an option; raw-byte corpora; ASan
fiber annotations with a before/after; the UBSan recover audit from the hosted log; a TSan model on the jobs suite
only; a diagnostic fuzz preset) and the increment ran directly. No hosted run was in flight; the latest published
run stays 34780682504 (`18651d5`).

## What changed

- [tests/support/fuzz](../../tests/support/fuzz/include/crd/fuzz/harness.hpp): the bounded harness (`BudgetAllocator`,
  `SeedSink`, `CRD_FUZZ_REQUIRE`, a fatal path without the Windows Debug CRT dialog) and the replay driver
  (`--seed`, `--verbose`, corpus directories, an empty corpus is a failure); [cmake/CrdFuzz.cmake](../../cmake/CrdFuzz.cmake):
  `crd_fuzz_target()`; four targets under `tests/execution/ceir/fuzz` and `tests/gpu/kir/fuzz` with their corpora
  (`.gitattributes`: raw bytes) and the `crd-fuzz-*-corpus` CTests; the `support/fuzz` test directory owned by the
  `ceir` and `kir` rows; [scripts/fuzz.py](../../scripts/fuzz.py) (`run`, `merge`, `minimize`, `adopt`, `replay`).
- Root options and policy: `CRD_ENABLE_FUZZER` (clang++ only), `CRD_ENABLE_TSAN` (GCC/clang, exclusive with ASan),
  `CRD_WARNINGS_AS_ERRORS` (ON everywhere but the diagnostic fuzz preset; the `/WX` and `-Werror` strings are
  unchanged, so `build/win-debug` is byte-identical: 47,781 Ninja lines, zero changed), UBSan with
  `-fno-sanitize-recover=undefined`, and `FATAL_ERROR` for every instrument a toolchain cannot provide.
- Preset `linux-clang-fuzz` (diagnostic tier; 23 visible presets) and its tier entry.
- [sanitizer_fibers.hpp](../../engine/foundation/jobs/src/sanitizer_fibers.hpp): the ASan and TSan model of the
  three fiber switch sites, the trampoline entry and the pool lifecycle; the deque's fence modelled as a seq_cst
  read-modify-write under TSan; the audit switch `CRD_JOBS_SANITIZER_FIBERS`.
- Loader fixes from the fuzz rounds (all rejections, none an exclusion): CEIR text exponent overflow, over-long
  operation names (`Context::kMaxOpNameBytes`, both loaders), an empty attribute name in the binary decoder, the
  brace lookahead accepting a quoted first attribute name, region nesting caps in both loaders; CKIR text `n_out`
  against `[[out]]`, bounded integer and node-ref accumulation; the CKIR blob reader now validates every bound the
  text reader validates (`serial_detail::refs_valid`) including the input count.
- Tests: three `TestInstruments` cases in `test-repository-tools.py`; docs: [design/test-instruments.md](../design/test-instruments.md),
  BUILDING (6,992 bytes), scripts README, CI-tiers design, ROADMAP rows 055, 057 (owner of the pending findings)
  and 067 (owner of the TSan findings); the pointer moves to REPO.DEV.10; three memory records.

## The fuzz rounds

`linux-clang-fuzz` (clang 18.1.3, ASan+UBSan, WSL2), two workers per target; each round ended when a worker found
an artifact:

| Round (per target) | ceir-text | ceir-binary | ckir-text | ckir-binary |
|---|---|---|---|---|
| 1 (600 s) | 1.29 M runs, 64 k/s, 2 artifacts, 26 s | 13 k runs, 2, 0.3 s | 182 k runs, 182 k/s, 1, 1.3 s | rebuilt before its board |
| 2 (300 s) | 1.56 M, 55 k/s, 2, 36 s | 676 k, 78 k/s, 2, 8 s | 1.58 M, 124 k/s, 2, 12 s | 668, 2, 0.1 s |
| 3 (300 s) | 1.98 M, 61 k/s, 2, 46 s | 108 k, 2, 1.7 s | 6.83 M, 96 k/s, 1, 288 s | 2.90 M, 143 k/s, 2, 41 s |
| 4 (120 s) | 153 k, 2, 3 s | 1.41 M, 94 k/s, 2, 23 s | 7.96 M, 77 k/s, 2, 111 s | 440 k, 168 k/s, 2, 3 s |

The 21 artifacts of rounds 1 to 3 were adopted (corpora 24, 16, 11 and 18 files): 12 root-caused to nine loader
defects and fixed as rejections; six never reproduced in isolation (three round-two CEIR reports, three
round-three CKIR reports found by a build that already carried every CKIR fix); three round-three CEIR reports
replay clean after the region caps and the lookahead fix with no traced cause. The eight of round 4 are committed
under `fuzz/pending/` (two stack overflows through attribute nesting under sanitizer-sized frames, a dangling
`string_view` in the CEIR binary decoder, two CKIR text writer/reader identity gaps, a span offset overflow in
the CKIR blob cursor) with REPO.DEV.11 as owner.
Peak RSS stayed between 0.3 and 1.5 GiB per worker under the 2 GiB limit; no timeout or out-of-memory artifact.

## The sanitizer boards

| Board | Result |
|---|---|
| `crd-jobs-tests`, `win-asan` (MSVC 19.51), annotated / unannotated | 22 of 22 / 22 of 22 |
| `crd-jobs-tests`, `linux-gcc-asan` (GCC 13.3, WSL2), annotated / unannotated | 22 of 22 / 22 of 22 |
| probe: one byte past a stack buffer on a fiber | stack-buffer-overflow on both compilers, both models |
| probe: use after return on a fiber (`detect_stack_use_after_return=1`) | GCC reports it, both models; MSVC does not (needs `/fsanitize-address-use-after-return`, unset) |
| probe: 2,000+ nested waits on four threads | clean everywhere |
| `crd-jobs-tests`, `CRD_ENABLE_TSAN` (GCC 13.3, `setarch x86_64 -R`) | 17 of 22; 16 reports, all `fiber_pool.cpp:296`/`:239` and `counter.cpp:66`/`:75` |
| probe under TSan | 2,048 waits clean; 68 reports at the same two free-lists |
| `crd-jobs-tests`, `win-debug` (no instrument) | 30 of 30 |

The hosted `linux-gcc-asan` job of run 34780682504 (15,918 log lines) carries no `runtime error:` line, so the
no-recover flag keeps the lane green. clang++ on Linux fails 139 translation units of the ceir and kir closure
under the GCC warning set (323 `-Wsign-conversion`, 222 `-Wnested-anon-types`, 13 `-Wdouble-promotion`), which is
why the fuzz preset shows warnings without `-Werror` and clang++ stays an unqualified compiler (REPO.DEV.11).
clang-cl on Windows was tried first: its ASan has no debug-CRT runtime and, through lld-link, its objects and the
runtime library disagreed on the runtime; dropped.

## Verification

- **Loader suites.** `crd-ceir-tests` 534 cases (12,518 assertions) and `crd-kir-tests` 311 cases (58,495
  assertions) pass on `win-debug` with every fix; every adopted artifact and every seed replays clean on MSVC
  19.51 and on clang 18 with ASan+UBSan (`crd-fuzz-*-corpus`: 4 of 4 on both hosts).
- **GCC under `-Werror`.** `linux-gcc-asan` with `CRD_MODULES=ceir;kir` (GCC 13.3, ASan, UBSan without recovery):
  the harness, the four targets, the loader fixes and both suites compile without a warning; `crd-fuzz-*-corpus`
  4 of 4; `crd-ceir-tests` 534 cases (12,518 assertions) pass with no sanitizer report. The first whole-suite
  run of `crd-kir-tests` (all 311 cases in one process, after the CEIR suite) ended after 198 passing cases with
  an ASan `SEGV` at a non-executable address inside the CKIR CPU evaluator during "B15-b cloud ray-march"
  (`ckir_kernel_eval.hpp:147`, a wild jump, no UBSan line); the case builds its graphs in code and never touches
  the changed loaders. It did not recur: 4 further whole-suite runs in one process (the working tree with
  `CRD_MODULES=kir`, twice plain and once with `detect_stack_use_after_return=1`, and a HEAD tree built the same
  way) all pass 311 cases (58,465 assertions), and the case passes alone on both trees. The hosted lane runs
  every case in its own process and has never run this combination. Recorded as an open instrument finding for
  REPO.DEV.11 (the log is in the session scratchpad; the same WSL session also produced one GCC internal
  compiler error during a parallel build, so the host is under suspicion too). The trees were removed afterwards.
- **Instruments.** The sanitizer boards above; the default `win-debug` graph unchanged by the warnings genex; the
  harness abort path proved on MSVC 19.51 (the pending CKIR text inputs end `crd-fuzz-ckir-text.exe` with exit
  code 3 in 0.07 s, no dialog; the other pending inputs pass on MSVC Debug without sanitizer frames).
- **Guards.** `check-ci-tiers.py` PASS (23 visible presets, 21 owned, 2 diagnostic); `test-native-build-profiles.py`
  7/7; `test-repository-tools.py` 46/46; `test-module-selection.py` 12/12, `test-project-sync.py` 55/55,
  `test-dev-workflow.py` 54/54; `check-repository.py` PASS; `check-pins.py` PASS; `check-master-plan.py` PASS;
  `git diff --check` clean.
- **Scratch.** The WSL trees (`~/tsan-jobs`, `~/asan-jobs`, `~/asan-jobs-off`, `~/fuzz`, `~/fuzz-scratch*`) and the
  Windows trees (`build/asan-jobs`, `build/asan-jobs-off`) were removed afterwards; the logs stay in the session
  scratchpad only.
