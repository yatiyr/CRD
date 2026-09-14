# Contribution routes, the ownership map and generated-source provenance

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.10](../ROADMAP.md#slice-repo.dev.10); contract:
> [CONTRIBUTING](../CONTRIBUTING.md). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [test instruments](2026-09-14-test-instruments.md).

## User direction

Unchanged: carry the REPO.DEV slices "one by one until AUD-2". The design gate ruled that the Windows rendering of
the FFT twiddle constants is a declared property of the generators, not a fix for this row (a correctly rounded
regeneration changes thousands of constants in a crush-mandated kernel and needs the FFT owner's qualification),
that the hosted lanes check hashes rather than regenerate (they install Python 3.12 without numpy), and that no
absent generator is recovered. No hosted run was in flight; the latest published run stays 34780682504 (`18651d5`).

## What changed

- [docs/CONTRIBUTING.md](../CONTRIBUTING.md): four compact routes (module, bug fix, asset/program, build-system
  change), the generated-source contract, the ownership route (module → registry row → systems-map row → owning
  ROADMAP row → the maintainer; no teams, no CODEOWNERS, no pull-request templates) and the human-only publication
  handoff. Linked from START_HERE (4,238 bytes), the docs README table and scripts/README.
- [Systems map](../systems/README.md): the six registered application and tool modules (`asset_cooker`, `ceridc`,
  `kir_autotune`, `shader_cook`, `runtime`, `sandbox`) get rows; the coverage sentence now says 102 registered
  modules. `check-master-plan.py` holds the map and the `crd_module()` registry to one set of names
  (`registered_modules`, `registry_map_errors`: missing, unregistered and duplicate rows fail).
- [scripts/generated-sources.json](../../scripts/generated-sources.json) names all 160 committed generated files
  (generator, status, LF-normalized SHA-256) plus one `not_generated` exclusion; [check-generated.py](../../scripts/check-generated.py)
  is the `crd-generated-sources` CTest and a repository-job step on both lanes (`--scan`, `--regenerate`,
  `--refresh`). The marker rule is line-leading, so prose that mentions generation does not match.
- [scripts/gen_license_manifest.py](../../scripts/gen_license_manifest.py) renders
  [dependency-licenses.md](../generated/dependency-licenses.md) from `cmake/pins.json` (10 packages, 11 tools,
  7 actions, 3 runner images, 12 unpinned apt packages, the asset licenses and the ignored `external/`); `--check`
  is the `crd-license-manifest` CTest and a lane step.
- Provenance comments on `hier_codelets.hpp` (generator absent), `aos_codelets.hpp` and `erk_tableaus.hpp`
  (drifted); no code line changed. Tests: five `Provenance` cases in `test-repository-tools.py` (51 cases).
- ROADMAP row 056 → Needs CI; row 057 (REPO.DEV.11) owns the generated-source follow-ups; the pointer moves to
  REPO.DEV.11; one memory record.

## Reproducibility measurements

Reference host: WSL2 Ubuntu 24.04, GCC 13.3 glibc, Python 3.12.3, numpy 2.4.6, scipy 1.17.1, pywt 1.8.0,
ml_dtypes 0.5.4, opt_einsum 3.4.0, cotengra 0.8.2, tensorly 0.9.0, tntorch and torch present.

| Generator | Committed file | Reference host result |
|---|---|---|
| `gen_fft_batched.py hybrid2` | `batched_codelets_gen.hpp` (14,020,588 B) | byte-identical in four runs; `hybrid`, `greedy`, `belady` ran clean (different schedules by design); no exception in seven runs, so the 2026-09-12 `AttributeError` in `score` did not recur |
| `gen_fft_codelets.py` | `codelets.hpp` | byte-identical |
| `gen_wavelet_coeffs.py` | `wavelet_coeffs.hpp` | byte-identical |
| `gen_ark_tableaus.py` | `ark_tableaus.hpp` | byte-identical (network-sourced from SUNDIALS) |
| `gen_aos_codelets.py` | `aos_codelets.hpp` | differs: 98,309 B against 57,899 B committed, 2,068 diff lines, the header was maintained by hand (`drifted`) |
| `gen_erk_tableaus.py` | `erk_tableaus.hpp` | differs in formatting only: 4,593 B one-line arrays against 13,781 B wrapped; same three tableaus and digits (`drifted`) |
| `ceir_opgen.py --check` | 104 files | ok |
| `gen_matrix.py --check` | capability matrix | ok |
| `v14a/b/d/e/i/j` corpus writers | six `ref_*.inc` | byte-identical in place |
| `v14k_tt_oracle.py` | `ref_tt.inc` | one constant differs in its ninth digit (`drifted`; tntorch/torch pair) |
| `v14g_export_corpus.py` | `ref_hyperopt.inc` | seven constants differ (`drifted`; randomized hyper-optimizer, no seed recorded) |

Windows (Python 3.14.4, numpy 2.4.6, no pywt): `gen_fft_batched.py` and `gen_fft_codelets.py` differ by one ulp in
their twiddle constants (11,802 lines for the batched header, `-0.7071067811865476` against the committed
`-0.7071067811865475`), because `cmath.exp` is the host's libm: both are `host-dependent`. `check-generated.py
--regenerate` on Windows: four generators ran, 53 entries skipped (in-place writers, host-dependent, pywt), zero
failures. Absent generators: `build/gen_subfft_m3.py` (hierarchical FFT), the four `hesap-special` minimax
polynomial scripts, and the scripts behind 34 reference-vector artifacts of the DSP, quadrature, special, stats,
wavelet and math suites (`gen_*_refs.py`; four artifacts name no script at all). Manifest census: 113 reproducible, 2 host-dependent, 1 network-sourced, 4 drifted, 39
generator-absent, 1 measured.

## Verification

- `check-generated.py` PASS (160 entries); `gen_license_manifest.py --check` PASS; `check-master-plan.py` PASS
  (863 rows, 1,051 documents, 9,147 local links) with the registry/map check; `check-repository.py`,
  `check-ci-tiers.py`, `check-pins.py` PASS; `test-repository-tools.py` 51/51 (three pre-existing skips);
  `test-module-selection.py` 12/12; `test-dev-workflow.py` 54/54; the workflow parses with the two new steps;
  `git diff --check` clean; orientation budgets held (START_HERE 4,238; AGENTS 7,971; BUILDING 6,992; context
  2,482; MEMORY 2,975).
- CTest registration proved on the reconfigured `build/win-debug` cache: `ctest -N` lists `crd-generated-sources`
  and `crd-license-manifest`, both pass (0.26 s and 0.04 s). Under Linux (WSL, Python 3.12.3, the checkout on the
  9p mount) `check-generated.py` passes in 18.9 s against its 120 s ceiling and the license check in under a
  second, so the LF-normalized hashes agree across hosts.
- No engine build was needed: the three header edits are comments; the guard hashes prove the bytes.

## Open, with owners

REPO.DEV.11 owns the correctly rounded twiddle constants and their regeneration (with the FFT owner's
qualification), the four drifted files, the 39 generator-absent artifacts recorded by hash, and the measured CKIR
tuning database. The hosted lanes cannot regenerate (no numpy); reference-host regeneration stays a local act
recorded in a session. The next push resolves to the complete tier (cmake, scripts, tests and the workflow changed).
