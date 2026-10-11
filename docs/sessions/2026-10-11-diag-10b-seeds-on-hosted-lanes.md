# DIAG.10b seeded detector failures on hosted lanes, 2026-10-11

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.10b](../ROADMAP.md#slice-diag.10b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-10b); route:
> [test instruments](../design/test-instruments.md#seeded-detector-failures-diag10b) and
> [CI tiers](../design/ci-tiers.md). Rules: [AGENTS](../../AGENTS.md). Preceding:
> [engine seams](2026-10-11-diag-10b-engine-seams.md).

## Goal

DIAG.10b's last local item: the eleven seeds of
[seeded-faults.json](../../tests/support/diag/seeded-faults.json) ran only on the reference host. This batch makes
them a hosted step on every lane they were qualified on, owned by the tier mapping, so a change that blinds a
detector fails CI. Committed locally, not pushed.

## The step

`.github/workflows/ci.yml` gains a `Seeded detector failures` step in the `windows` job (win-debug, win-asan) and the
`linux-gcc` job (linux-gcc-debug, linux-gcc-asan, linux-clang-tsan), after the lane's Test step:

```
python scripts/diag-seeded-faults.py --build-dir build/<preset> --preset <preset> \
  --trace-dir build/<preset>/evidence/diag-seeded-faults
```

- It runs with the implicit `success()`: only when the lane built and its tests (with the register gate) passed. A
  failing lane is already red, and its detecting tests would only end the seeds in CONTROL_FAILED.
- `--preset` makes the test preset's environment the base of every run (linux-clang-tsan's `TSAN_OPTIONS` with the
  third-party suppressions; linux-gcc-asan's `UBSAN_OPTIONS`); the runner's `ctest --test-dir` would not apply it.
- The runner's exit is the verdict: 0 only when every applicable seed is DETECTED, and ZERO_RUN when none applies.
  Nothing is rerun and there is no `continue-on-error`.
- The trace directory is under `build/<preset>/evidence`, which the lane's `evidence-<preset>` artifact already
  uploads: `report.json` and, per seed, the control, seeded and restored build logs, CTest logs and JUnit files.
- Each leg runs in its own preset's tier: the change tier on win-debug, win-asan, linux-gcc-debug and linux-gcc-asan,
  the complete tier on linux-clang-tsan. Each seed rebuilds only the targets holding its detecting tests.

## The lane's own test record

The seeds' CTest runs use the lane's build tree, and CTest rewrites `Testing/Temporary/Last*.log` on every run. The
first full local runs showed the effect: after them `build/win-debug/Testing/Temporary/LastTestsFailed.log` still
named the symbol seed's test, and `LastTest.log` held the last restored run. The hosted evidence artifact uploads
`build/**/Testing/Temporary/*.log`, so the lane's record would have carried a seeded failure. The runner now keeps
that record (`LaneTestRecord`): it saves every `Last*.log` before the first seed, and afterwards writes each back
byte for byte and removes any the seeds created, also when a seed raises. The seeds' own CTest logs and JUnit stay
in the trace directory. Rerunning the win-debug step after the change left both files' MD5 sums unchanged and no
`LastTestsFailed.log`.

## The mapping and its validator

The step is a `preset_steps` entry with a list, `{"presets": ["win-debug", "win-asan", "linux-gcc-debug",
"linux-gcc-asan", "linux-clang-tsan"]}`, beside the GenMC step's single `{"preset", "tier"}` form.
`check-ci-tiers.py` now accepts either form: a list step names each preset once and declares no tier of its own;
in each listed preset's job the step must exist and be gated on that preset, and a gate naming a preset the mapping
does not list fails. A new `test-repository-tools.py` case covers each rule against the real workflow: a dropped
gate, an extra gated preset, the step missing from one job (once per listed preset of that job), duplicate and
empty lists, a list with a tier, a single preset without a tier and a diagnostic preset in the list.

`test-diag-seeded-faults.py` no longer hardcodes five lane fact sets. It reads the step's presets from the mapping,
derives each lane's facts from its configure preset (cache variables through `inherits`, the compiler), and requires
every seed to apply to some hosted lane and every hosted lane to run seeds: 3 on win-debug and linux-gcc-debug, 5 on
win-asan, 6 on linux-gcc-asan and linux-clang-tsan.

## The exact step command on the local lanes

Run one lane at a time with the hosted command line (Windows inside vcvars; WSL with `--jobs 8`, linux-clang-tsan
under `setarch -R` as this host needs):

| Lane | Seeds applied | Verdict | Wall time |
|---|---|---|---|
| win-debug | redzone, dump, symbol | 3 DETECTED | 16 s |
| win-asan | specimens, ASan log file, `continue_on_error`, redzone, symbol | 5 DETECTED | 92 s |
| linux-gcc-debug | redzone, crash record, symbol | 3 DETECTED | 13.5 min (one CMake reconfigure over the 9p mount, then incremental builds) |
| linux-gcc-asan | specimens (4 tests), whole lane (4), ASan log file (2), LSan suppression, redzone, symbol | 6 DETECTED | 35 min (each CMakeLists seed reconfigures twice over the 9p mount) |
| linux-clang-tsan (`setarch -R`) | specimens, whole lane, TSan log file, TSan suppression, redzone, symbol | 6 DETECTED | 36 min (as above) |

Every seed's restore left git reporting the seeded files as before; `git status` after each lane showed only this
batch's changes. On a hosted lane the checkout is LF for every seeded file (`.gitattributes` gives `*.cpp`,
`*.txt` and `*.cmake` `eol=lf`), so the seeded texts match there as here; a mismatch would end REGISTRY_STALE, never a
silent pass.

## Teeth

- `check-ci-tiers.py`: removing the undeclared-gate check, the per-preset gate check, or the list-form shape check
  each failed the new case; restored, both preset-step cases and `check-ci-tiers.py` pass.
- `LaneTestRecord`: not removing the files the seeds created, or not writing the saved ones back, each failed the
  new verdict case; restored, the 33 cases pass.
- The seeds themselves are the teeth of the hosted step: each DETECTED verdict above is a seeded build whose named
  tests failed with the named text, followed by a restored build that passed.

## Verification

- The five lanes above, with the hosted command line; `git status` after each showed only this batch's files.
- `check-ci-tiers.py` PASS; `test-repository-tools.py` 66 cases (the 3 skips need CMake and Ninja on PATH and ran in
  the `crd-repository-tools` CTest); `test-diag-seeded-faults.py` 33 cases (Windows and WSL Python); `test-dev-workflow.py` 54 cases.
- The pinned actionlint 1.7.12 on `ci.yml` (`-shellcheck= -pyflakes=`) on WSL: clean.
- win-debug CTests: the 8 `crd-no-*`/`crd-check*` guards, `crd-ci-tiers`, `crd-diag-seeded-faults`,
  `crd-diag-coverage`, `crd-repository-tools`, `crd-developer-workflow`, `crd-master-plan` and
  `crd-repository-hygiene`.
- No C++ source changed, so no lane build beyond the seeds' own incremental builds was needed.
- check-master-plan and check-repository PASS.

## Remaining in DIAG.10b

Only the hosted run. It must show the `Seeded detector failures` step green with `report.json` DETECTED for every
applicable seed on win-debug (3), win-asan (5), linux-gcc-debug (3), linux-gcc-asan (6) and linux-clang-tsan (6,
complete tier), and the row's earlier hosted requirements.
