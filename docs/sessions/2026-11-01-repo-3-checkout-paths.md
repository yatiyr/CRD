# 2026-11-01 — REPO.3: scripts find the repository from their own location; literal checkout paths refused

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Rows: [REPO.3](../ROADMAP.md#slice-repo.3), the literal-path item the
[CUDA failure-path note](2026-11-01-repo-3-cuda-failure-path.md#remaining-on-repo3) left;
[MCP-ADAPT.0](../ROADMAP.md#slice-mcp-adapt.0)'s hosted evidence. Track B, light work on the Windows host; the
`cerid-track-b` machine ran only to probe its Python modules.

## CI first

The `workflow_dispatch` run 41274380605 at de5e10b0 has finished every Windows lane. Its failures, each read from
the job's log:

- **win-debug** (job 124764040316, MSVC): one failure out of 9,392, `filesystem: the last write time converts
  exactly and orders files within one second` (`test_filesystem.cpp:145`), as on win-debug-scalar, win-public-checks,
  win-asan shard 2, win-shipping, win-shipping-profile and win-release; fixed on main by cc5ee9e9 (after de5e10b0).
  All 31 cases named for `ceridc`, MCP or the protocol stream pass (`crd-ceridc-mcp-conformance` and
  `platform: the protocol owns its output stream and stray writes go to stderr` included), as on win-debug-scalar
  (124764040380, 31 of 31). win-asan shard 1 (124764040425) passes all 4,697 of its cases, 14 of them such cases.
- **win-tidy** (124764040287): `readability-identifier-naming` on `kUnixNs` in `test_filesystem.cpp:134`; fixed on
  main by 4b9bc837.
- **win-clang-cl** (124764040296) and **win-clang-cl-shipping** (124764040255): `lld-link` cannot find `__udivti3`
  and `__divti3` for `crd-perf-tests.exe`; fixed on main by 7cfed26c. So no clang-cl lane has tested the row's
  cases yet.
- **linux-gcc-release** (124764040376) and **linux-gcc-shipping** (124764040299): GCC 13's LTO
  `-Wmaybe-uninitialized` on `test_multivariate.cpp:125`; fixed on main by 76330344.
- **win-release** (124764040325), a second failure that is not this track's and is not fixed on main:
  `diagnostic host: the application's wild write leaves one attributable crash capture, and the repaired one runs
  clean` (`test_diag_host.cpp:1838`, 1839, 1853, 1858 and 1859) reports `code image: the module is not a Windows
  image` (`fault_classification.cpp:936`), so the code-image comparison is `0` instead of `Matches` and the instruction
  judgement follows it. It is HW-RESILIENCE.2's crash-capture code (track A); no other finished Windows lane failed
  it. Recorded here for track A.

Each push replaces the push group's pending run; this batch's push replaces the pending one at a936e7b7. A second
`workflow_dispatch` run, 41295647725 at 43963cb2 (later than 7cfed26c), was queued at 10:02 UTC; its win-clang-cl
lane is the first that can test MCP-ADAPT.0's cases on clang-cl.

## Literal checkout paths

Ten scripts opened a path that exists on one machine:

- **The tensor corpus generators** `v14a_dtypes_corpus.py`, `v14b_elementwise_corpus.py`, `v14e_einsum_corpus.py`,
  `v14g_export_corpus.py`, `v14j_decomp_oracle.py`, `v14m_nn_oracle.py` (writers) and `v14m_nn_peers.py`,
  `bench_einsum_plan.py` (readers) named `/mnt/d/Dev/cerid/tests/numerics/hesap-tensor/...`. Each now joins the
  path to its own file's directory, the form `v14d_permute_corpus.py` already used, so a run from any directory
  and any checkout reads and writes the repository's files. The five text writers open their `.inc` with
  `newline="\n"`, as `v14d` does, so a Windows run writes the committed LF bytes instead of CRLF. `bench_einsum_plan.py`
  closes the file it reads.
- **The MATLAB boards** `v14_matlab_board.m` and `v14_matlab_ttb.m` wrote `D:/Dev/cerid/scripts/...`: the results
  file is now `fullfile(fileparts(mfilename('fullpath')), ...)`, beside the script. The TTB runner added
  `C:/Users/<user>/tools/tensor_toolbox` to the path; it now uses the toolbox already on the path, or the folder
  `CRD_TENSOR_TOOLBOX` names, and stops with the board's own "not on path" message when there is none.
- **Usage comments** in `setup-wsl-deps.sh`, `wsl-build.ps1` and `v15_ad_oracle.py` spell the commands relative to
  the checkout; `workspace.py`'s project-key docstring uses two of the keys its test observed.

**The guard.** `check-repository.py`'s text pass (`text_file_errors`, formerly `text_encoding_errors`: one read of
each tracked text file) refuses a literal checkout path, `<drive>:` or WSL's `/mnt/<drive>` followed by `Dev/cerid`
in any slash or backslash form and any case, the `cerid-*` worktrees included, naming the file, the line, the count
and the first spelling. Markdown and `docs/` are the record (session notes and the memory name the checkouts they
ran in) and `scripts/test-*.py` fixtures are sample text, so neither is read. `test_checkout_paths` covers six
spellings, the line and count, an engine source, the exempt classes and six near misses (`~/cerid-src`, a home
directory, `D:/Dev/nav/cerid`, `/dev/cerid` and others). Teeth: with the new guard over the old scripts,
`check-repository.py` reports 14 files (the ten above and the four commented ones).

**Evidence.** `python -m py_compile` on the eight Python scripts; `check-repository.py`, `check-master-plan.py`,
`check-allman-braces.py`, `check-generated.py` pass, as do ten of `test-repository-tools.py RepositoryTools`'s eleven cases (the
eleventh, `test_generated_target_folders`, configures a CMake tree with Ninja, which this track does not run on
the Windows host; the code it covers is unchanged). The generators
were not rerun: the track machine's CI image has none of numpy, ml_dtypes, opt_einsum, tensorly, cotengra or torch,
and the repository has no pinned, hash-checked way to install them; the change is the path expression only, the
same one the reproducible `v14d` generator uses. (`nn_corpus/`, the torch export `v14m_nn_oracle.py` writes, is not
in `generated-sources.json`; whether torch's ONNX export is byte-reproducible is unproven, so no status is claimed.)

## Remaining on REPO.3

- The run on the measuring PC's NVIDIA GPU: `bash scripts/run_bench_gpu_cuda.sh` under WSL2 on that PC (a build and
  run this track may not do there), its rows and ratios against the recorded boards.
- The LunarG Qt `setPixelSize` line from a cold Windows lane.
- The close after [REPO.3d](../ROADMAP.md#slice-repo.3d).
