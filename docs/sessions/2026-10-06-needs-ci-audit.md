# Needs CI audit against the first green complete-tier run, 2026-10-06

<!-- doc-role: historical -->
> Dated evidence. Live owners: [REPO.DEV](../ROADMAP.md#slice-repo.dev) and its children,
> [DIAG.3a](../ROADMAP.md#slice-diag.3a), [DIAG.3b](../ROADMAP.md#slice-diag.3b). Rules: [AGENTS](../../AGENTS.md).

## Why this audit

The user asked why 32 rows still read Needs CI after days of CI work. The roadmap rule is "inspect changed CI results
first on re-entry". It was not followed: each CI read looked only for failures, and no row was rechecked once its
evidence existed.

That evidence came with run [37311671924](https://github.com/yatiyr/CRD/actions/runs/37311671924) at `4737877d`
(2026-10-05). It is the first complete-tier hosted run in which **all 25 jobs passed**:
- preflight, which resolved the run to the complete tier;
- the two repository jobs;
- the 21 test lanes, including the five presets that had never run hosted (`win-relwithdebinfo`, `win-debug-scalar`,
  `win-shipping-profile`, `linux-gcc-debug-scalar` and the native `win-vs`) and both public-check presets;
- `required`.

Each row below was checked against the list in the
[infrastructure audit](2026-09-14-infrastructure-audit.md) of what "the first hosted run must show".

## What the run shows

- **Repository jobs, on both runners with Python 3.12:** every guard passes (`check-repository`, `check-master-plan`,
  `check-ci-tiers`, `check-pins`, `check-generated` over 160 sources, `check-allman-braces`, the license manifest).
  So do the tooling suites:
  - `test-repository-tools` (55 tests);
  - `test-module-selection` (12);
  - `test-dev-workflow` (54);
  - `test-project-sync` (60);
  - `test-native-build-profiles` (7; on Windows also under Visual Studio 18 2026);
  - the native project sync, and the CEIR generator and capability-matrix checks.
- **Evidence bundles:** each of the 21 lanes uploaded one. The `win-debug` bundle's `conclusion.json` (schema
  `cerid-ci-evidence/1`) records revision `4737877d`, the tree, the toolchain, the adapter and the build's `ninja` and
  `sccache` sections. `win-debug` built through the pinned sccache: 1,607 hits, 97.3 %.
- **Fuzz corpora:** the four loader corpora and the perf-bundle corpus ran and passed on every lane checked.
  `linux-gcc-asan` ran its whole suite; its one failure is the registered third-party defect TP-5, accepted by the
  register gate.
- **Verified acquisition:** the Vulkan headers ("every digest verified"), the Khronos validation layer and sccache
  ("archive SHA-256 verified") were downloaded and verified on the lanes that install them.
- **Not shown: cold CPM acquisition.** Every lane restored its CPM archive cache (and Windows its Vulkan SDK cache),
  through the restore-key fallback. The first complete run, `34821419392` on 2026-09-14, restored them too. No hosted
  run has yet acquired the CPM archives, the Vulkan SDK and WARP from cold through the verifying helpers.

## Verdict per row

| Row | The hosted run had to show | Shown in 37311671924 | New state |
|---|---|---|---|
| REPO.3c | a green complete run over its closed children | yes | Done |
| REPO.DEV.3b.3 | the hosted repository jobs | yes, both runners | Done |
| REPO.DEV.3b, 3c, 3 | `test-dev-workflow.py` (54) and the frontend guards on both runners | yes | Done |
| REPO.DEV.4 | `test-module-selection.py` (12) and `check-repository.py` on both runners | yes | Done |
| REPO.DEV.5 | preflight resolving to complete; `required` green with every expected job; a bundle per lane; the five presets and the native solution | yes | Done |
| REPO.DEV.6 | cold runners acquiring every archive, the Vulkan SDK and WARP through the verifying helpers | **no**: every CPM and SDK cache was restored | Needs CI (named gap) |
| REPO.DEV.7 | each bundle with its Ninja log and sccache statistics; `win-debug` built through sccache | yes | Needs CI: waits on its prerequisite DEV.6 |
| REPO.DEV.8 | both public-check presets green | yes | Needs CI: waits on DEV.6 through DEV.7 |
| REPO.DEV.9 | the corpus CTests on every lane; `linux-gcc-asan` whole suites | yes | Needs CI: waits on DEV.6 through its prerequisites |
| REPO.DEV.10 | the two repository-job steps on both runners with Python 3.12 | yes | Needs CI: waits on DEV.6 through its prerequisites |
| REPO.DEV.11 | the first complete-tier run, and the selector comparison | the run, yes; the comparison needs a complete run of a change-tier revision (the first change-tier push is `215813f0`) | Needs CI |
| REPO.DEV (parent) | every child | DEV.6 to 11 remain | Open |

## Rows that never belonged in Needs CI

Needs CI requires that all available local work is finished. Two rows say otherwise in their own text:
- **DIAG.3a:** "the remaining allocators' size paths + the full per-allocator zero-size/exhaustion/realloc-failure
  sweep".
- **DIAG.3b:** "the intentional use-after-reset negative-control specimen".

Both are now Partial, with that work named. The other DIAG Needs CI rows (DIAG.1a to DIAG.6b) cannot become Done yet:
they sit after the open REPO.DEV and DIAG.0, and the validator allows Done only in table order. They keep their CI
wait.

## Closing DEV.6

The gap needs one hosted run that cannot restore an archive cache. There are two ways to get it:
- a manual complete run after the repository's Actions caches are cleared, which is the user's decision; or
- a standing cold-acquisition step that configures with an empty CPM source cache in the complete tier.

Either one also closes DEV.7 to DEV.10, whose own hosted clauses are already shown.

**User decision (2026-10-06):** "go with option 2", the standing step.
- **Landed:** the complete-tier `win-debug-scalar` and `linux-gcc-debug-scalar` now skip every archive cache, and
  `scripts/check-cold-acquisition.py` checks before and after the configure.
- **Verified locally:** a real cold configure on the WSL reference host acquired all seven selected packages; the
  checker passed, and its `--before` mode rejected the warm cache afterwards. The tooling suite (56 tests) and the
  pins, CI-tier and repository guards pass.
- **Remaining:** the next complete-tier hosted run is the proof.
