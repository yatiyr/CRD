# Infrastructure audit: the REPO.DEV children against their measured acceptance

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.11](../ROADMAP.md#slice-repo.dev.11); parent close:
> [REPO.DEV](../ROADMAP.md#slice-repo.dev). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [contribution and provenance](2026-09-14-contribution-and-provenance.md).

## User direction

Unchanged: carry the REPO.DEV slices "one by one until AUD-2". The design gate ruled that every finding this row
owns leaves it with one disposition (fixed, reclassified as a declared property, or routed to a row outside the REPO
tree that will resume) so that nothing stays parked under a REPO row; that the audit cites each child's own evidence
rather than re-running it (a re-run of every child's checks is the local sweep the rules forbid); and that no fifth
fuzz round is run. No hosted run was in flight; the latest published run stays 34780682504 (`18651d5`), which ran the
workflow before the tiers contract. The next push resolves to the complete tier and is the first hosted run of the
tiers workflow, the public-check presets, the corpus tests and the provenance steps.

## Part A: the children against their acceptance

Rows 047 and 048 (REPO.DEV.3b, 3c) closed under the [frontend qualification](2026-09-13-frontend-qualification.md)
and carry the same hosted wait as REPO.DEV.3. Every row below is Needs CI on the cited local evidence.

| Child | Contract | Local evidence (cited) | The first hosted run must show | Retained gates |
|---|---|---|---|---|
| [DEV.3](../ROADMAP.md#slice-repo.dev.3) frontend | doctor/plan/check/evidence, affected-target selection with conservative fallbacks | Windows `check` 65/65 (ceir-gpu-dx12), 206/206 (jobs, perf), 539/539 (header repairs); Linux `check` 120/120 after the guard budget rose to 300 s; the 60 s Git budget over 9p ended as `instrument_failure`, no evidence claimed ([session](2026-09-13-frontend-qualification.md)) | the repository job's `test-dev-workflow.py` (54 cases) and the frontend guards green on both runners | a hosted Linux strict tidy lane is REPO.DEV.5's matrix question, not a frontend defect |
| [DEV.4](../ROADMAP.md#slice-repo.dev.4) module registry | one registry, declared edges, `CRD_MODULES` closure, verifier, host tools | full `win-debug` graph identical (292 targets, 1,814 compile commands, 5,786 Ninja statements); `hesap-fft` alone: 9 of 102 modules, 36 Linux CTests; `sandbox` with imported host tools: 63 of 102; cook pack 100,272,336 bytes byte-identical on recook ([session](2026-09-14-module-registry.md)) | `test-module-selection.py` (12 cases) and `check-repository.py` in the repository job on both runners | a hosted module-subset configure is not in the matrix; the local closure proofs stand |
| [DEV.5](../ROADMAP.md#slice-repo.dev.5) CI tiers | every preset owned, tier from changed paths, `required`, evidence bundles | `check-ci-tiers.py` PASS; resolver on real revisions and synthetic sets; `required` cases; the five never-hosted presets configured locally (`win-relwithdebinfo` 102 of 102 modules in 30.7 s) ([session](2026-09-14-ci-tiers.md)) | preflight resolving to complete, `required` green with every expected job, a `conclusion.json` bundle per lane, the five presets and the native solution building and testing hosted | the change tier has not been exercised by any repository commit (Part B); shard counts and parallelism wait for the first durations |
| [DEV.6](../ROADMAP.md#slice-repo.dev.6) pinned inputs | one registry of versions, sources, SHA-256 and licenses; verified acquisition; pinned actions and runners | `check-pins.py` PASS; `win-debug` graph identical except the patched imgui path; Linux reconfigured with verified archives (201.8 s over 9p); one source cache unchanged across four consumers ([session](2026-09-14-pinned-inputs.md)) | cold runners acquiring every archive, the Vulkan SDK and WARP through the verifying helpers | none beyond the hosted acquisition itself |
| [DEV.7](../ROADMAP.md#slice-repo.dev.7) build board | measured board, opt-in launcher and pools, default graph unchanged | MSVC PCH off 137.4 s against on 136.5 s; sccache miss +20 %, full hit 43.5 s (3.1×); GCC hit 20.3 s (7.0×); graph identical ([session](2026-09-14-build-performance.md)) | every lane's bundle carrying its Ninja log and sccache statistics; `win-debug` hosted with PCH off | Shipping and Visual Studio generators stay truthfully uncached |
| [DEV.8](../ROADMAP.md#slice-repo.dev.8) public consumption | self-contained public headers through the consumer view; relocatable package | 1,118 of 1,118 MSVC shims and 1,110 of 1,110 GCC shims after 21 header fixes; consumer package built and refused on profile mismatch on both hosts ([session](2026-09-14-public-consumption.md)) | `win-public-checks` and `linux-gcc-public-checks` green in the complete tier | none; the exclusion API has no entries |
| [DEV.9](../ROADMAP.md#slice-repo.dev.9) instruments | bounded fuzz targets with committed corpora, sanitizer policy, fiber annotations | 29 artifacts over four rounds, 21 adopted with nine loader fixes; corpora replay on MSVC, clang and GCC; UBSan without recovery; TSan names the two free-list races ([session](2026-09-14-test-instruments.md)) | the four corpus CTests (now 26, 18, 13 and 20 inputs) on every lane; `linux-gcc-asan` whole suites case-per-process | libFuzzer stays a local diagnostic preset; no TSan preset (CORE-USE.2 owns the races) |
| [DEV.10](../ROADMAP.md#slice-repo.dev.10) provenance | contribution routes, ownership route, generated-source manifest, license manifest | guard PASS on both hosts (160 entries; 0.26 s CTest on Windows, 17 to 19 s over 9p); license check PASS; registry and systems map held to one set of names ([session](2026-09-14-contribution-and-provenance.md)) | the two repository-job steps green on both runners with Python 3.12 and no numpy | regeneration stays a reference-host act |

## Part B: selector against full CI

The local half is the resolver replay of `scripts/ci-tier.py resolve` over the recent history. Every repository
commit since 2026-09-12 resolves to the complete tier (each touched `cmake/`, `scripts/`, `.github/` or the
register) and the CEIR-era source commits before them resolve to the change tier. The failing revision `a0419cf`
(run 34766787633, job `linux-gcc (linux-gcc-debug)`) resolves to complete with all eight jobs, `linux-gcc-debug`
among them: the selector would have run the lane that failed. So far no repository commit has exercised the change
tier, and the tiers workflow itself has never run hosted; the two published runs used the previous workflow.

The hosted half is a procedure the first runs must satisfy, not a number this session can produce: for every push
that resolves to the change tier, the nightly (03:00 UTC) or manual complete run of the same revision must not fail
a lane the push skipped. The research's acceptance holds unchanged: a broad comparison must show no missed required
test before selection replaces a full gate, and the explicit full mode (dispatch `tier=complete`, the nightly) is
retained thereafter. The comparison table starts with the first change-tier push and its complete run.

## Part C: unattended recovery

No new code; the contracts and their tests already cover the failure classes an unattended run meets.

- **Native project synchronization.** Every CLI delete journals the bytes first and `project-sync.py recover
  <transaction-id>` restores them; native IDE deletions before synchronization are declared unrecoverable by the tool
  ([design](../design/project-structure-sync.md); `test-project-sync.py` 55 cases, `test-project-sync-native.py` on
  the Visual Studio generator).
- **The frontend.** A `check` that cannot measure ends as `instrument_failure` and claims nothing (the 60 s Git budget
  over 9p in the [qualification](2026-09-13-frontend-qualification.md)); the strict gate's guard budget is 300 s.
- **The lanes.** `required` fails on a skipped expected job or a failed preflight, cancellation is limited to
  superseded pull-request runs, and a diagnostic preset in a hosted job fails the mapping check
  ([CI tiers](../design/ci-tiers.md)).
- **The loop.** Every tick polls the hosted run first and a red lane preempts slice work; the agent re-anchors to the
  task's purpose at every prompt and no historical grant restarts a stopped loop ([AGENTS](../../AGENTS.md)).

## Part D: dispositions of the owned findings

### The eight round-four fuzz artifacts

The `linux-clang-fuzz` harness (clang, ASan+UBSan without recovery, `CRD_MODULES=ceir;kir`) was rebuilt from the
working tree and each pending artifact replayed alone, then the six that passed were replayed 5,000 times each in one
process (the libFuzzer finding came from a long-lived process).

| Artifact | Target | Round-four symptom | Fresh replay | 5,000 in-process repetitions | Disposition |
|---|---|---|---|---|---|
| `0add989c` | ckir-text | serialize mismatch after re-read | oracle failed at byte 515 of the blob (`0x80` against `0x00`) | not needed | fixed: `ckir_write` elided `cval` through `!= 0.0`, which is false for `-0.0`; the elision now tests the bit pattern |
| `dd98295f` | ckir-text | writer's output rejected, "bad node ref" | reproduced at byte 648: `node = "n-1"` in `[[out]]` | not needed | fixed: a `[[out]]` block without `node =` left the output at `-1`; the text reader and the blob reader now reject an output that names no node ("stage output names no node") |
| `553e7ecf`, `d85628dd` | ceir-text | stack-overflow-shaped reports (a jump to a stack address; an unknown-crash in `parse_type`) | pass (1.9 ms, 0.7 ms) | 10,000 inputs clean in 4.2 s | adopted as corpus inputs; both are near-copies of the seed with no deep nesting |
| `183a3e2e`, `e18eb14e` | ceir-binary | stack-buffer-underflow through a `string_view` comparison; a misaligned `string_view` pointer | pass (2.5 ms, 0.5 ms) | 10,000 inputs clean in 9.0 s | adopted as corpus inputs |
| `34ce93c0`, `c7c59902` | ckir-binary | unsigned offset subtraction overflow; stack-buffer-underflow in `Cursor::u8v` | pass (0.1 ms each) | 10,000 inputs clean in 1.5 s | adopted as corpus inputs |

Regression: `test_ckir_asset.cpp` gains the `-0.0` round trip and the output-without-node rejection. The blob
reader's `deserialize_graph` applies the same output rule. No `fuzz/pending/` directory remains; the policy for a
process-state finding that does not replay in isolation is recorded in [test instruments](../design/test-instruments.md).

### The GCC ASan whole-suite `SEGV`

Closed as not reproduced: one observation in `eval_cpu_kernel` during "B15-b cloud ray-march", five clean
whole-suite runs afterwards (three working-tree runs including `detect_stack_use_after_return=1`, one HEAD-tree run,
and the case alone), no defect established. The hosted `linux-gcc-asan` lane runs each case in its own process.

### clang++ on Linux

The keep-going `-Werror` build of the `ceir;kir` closure under the fuzz preset reported 558 diagnostic lines across
139 failed translation units of 352. Deduplicated by site they are 40 warnings in 13 files:

| Class | Sites | Where |
|---|---|---|
| `-Wsign-conversion` | 25 | `engine/gpu/kir` 15 (`ckir_kernel_eval.hpp`, `ckir_glsl.hpp`, `backend.hpp`), `tests/gpu/kir` 9, `engine/foundation/core` 1 (`crash.cpp`) |
| `-Wdouble-promotion` | 13 | `engine/gpu/kir` (`ckir_kernel_eval.hpp`) |
| `-Wnested-anon-types` | 2 | `engine/foundation/containers` (`string.hpp`, repeated into every including unit) |

Disposition: clang++ on Linux stays a diagnostic compiler (`linux-clang-fuzz`, `CRD_WARNINGS_AS_ERRORS=OFF`), recorded
in the [CI tiers](../design/ci-tiers.md) contract; the warnings are not fixed here (the sign and promotion sites are
numerics decisions in the CKIR evaluator and emitters). Whether a qualified clang++ lane joins the matrix is a cost
question for the maintainer.

### Generated sources

Statuses other than `drifted` are declared properties of a file and no longer carry an owner (44 owner fields
removed from the manifest; `check-generated.py` enforces the owner only for `drifted`).

| File | Was | Now | Evidence |
|---|---|---|---|
| `erk_tableaus.hpp` | drifted (wrapped arrays) | `formatted` | the generator's output through the repository's clang-format reproduces the committed bytes: pinned LLVM 20.1.8 on Windows and the Visual Studio 20.1.8 agree (SHA-256 `05e4f41e…`); `--regenerate` applies the formatter (5 generators ran on Windows, byte-identical) |
| `aos_codelets.hpp` | drifted | `hand-maintained` | the header is the truth (98,309 generated bytes against 57,899 committed, a different butterfly formulation); its header comment says so |
| `ref_tt.inc`, `ref_hyperopt.inc` | drifted, owner REPO.DEV.11 | drifted, owner [HGP-4](../ROADMAP.md#slice-hgp-4) | the tensor-and-autodiff row decides seed recording and the tntorch/torch pair when it resumes |
| two FFT twiddle headers | host-dependent, owner REPO.DEV.11 | host-dependent, no owner | a correctly rounded rendering is the FFT owner's qualification when that programme resumes |
| 39 artifacts | generator-absent, owner REPO.DEV.11 | generator-absent, no owner | the bytes are the artifact, recorded by hash |
| `ckir_tuning_db.inc` | measured, owner REPO.DEV.11 | measured, no owner | regeneration needs the adapter |

Census after the audit: 113 reproducible, 1 formatted, 2 host-dependent, 1 network-sourced, 1 hand-maintained, 2
drifted (HGP-4), 39 generator-absent, 1 measured.

### Retained gates, stated once

The first complete-tier hosted run (every Needs CI row, Part A); adapters and platforms this machine lacks (the
hosted NVIDIA and WARP identities in the conclusion bundles, macOS and web); the human publication boundary (the
maintainer commits and pushes; the loop polls). Nothing else is owned by a REPO row.

## Verification

- Fuzz: `linux-clang-fuzz` rebuilt (127 steps); pending replay 6 pass, 2 fail before the fix, 8 pass after; the
  four corpus CTests pass under the sanitizer build (0.30 s) and on `win-debug` after the rebuilt replay executables
  (0.28 s, zero diagnostics).
- CKIR: `crd-kir-tests "[asset]"` on `win-debug` (MSVC 19.51): 39 cases, 5,141 assertions, including the two new
  cases; the whole owning suite afterwards: 312 cases, 58,498 assertions, all passed (the blob reader's new output
  rule breaks no fixture). The fuzz compiler is Ubuntu clang 18.1.3.
- Provenance: `check-generated.py` PASS on Windows and on the reference host (17.0 s over 9p); `--regenerate` on
  Windows: 5 generators ran, 52 entries skipped, 0 failures, the ERK header byte-identical through the pinned
  clang-format; `test-repository-tools.py` 51 cases (three pre-existing skips) with the new status cases.
- Validators: see the close-out lines of the report (`check-master-plan.py`, `check-repository.py`,
  `check-ci-tiers.py`, `check-pins.py`, `gen_license_manifest.py --check`, `git diff --check`, orientation budgets).
- Scratch: the WSL fuzz tree, the repetition directories, the probe binary and the formatter comparison files were
  removed after the measurements.

## Open, with owners

Nothing is owned by a REPO row. HGP-4 owns the two tensor references; the FFT programme owns the twiddle rendering
and any AoS regeneration; CORE-USE.2 owns the free-list races; the maintainer owns the clang++ lane question and the
next push.
