# REPO.DEV.6 follow-up: every workflow action on Node 24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.6](../ROADMAP.md#slice-repo.dev.6). Contract:
> [pinned inputs](../design/pinned-inputs.md). Registry: [`cmake/pins.json`](../../cmake/pins.json). Rules:
> [AGENTS](../../AGENTS.md).

## The warning

Every hosted Windows job of complete run 40274818980 printed GitHub's annotation that Node.js 20 is deprecated and
that `actions/cache`, `actions/checkout`, `actions/setup-python`, `actions/upload-artifact`, `ilammy/msvc-dev-cmd` and
`seanmiddleditch/gha-setup-ninja` were being forced onto Node.js 24. A third-party warning, so repaired, not accepted.
`KyleMayes/install-llvm-action` runs on Node 20 too; it is used only by the clang-tidy job, so it was not in the
quoted list.

## What each action became

Releases from `gh api repos/<owner>/<repo>/releases`, each tag resolved to its commit through `git/ref/tags/<tag>`
(all five are lightweight tags, so the ref is the commit), and `runs.using` read from each candidate's `action.yml`.

| Action | Before | After | Runtime |
|---|---|---|---|
| actions/checkout | v4 `11d5960a` | **v7.0.1** `3d3c42e5aac5ba805825da76410c181273ba90b1` | node24 |
| actions/setup-python | v5 `a26af69b` | **v7.1.0** `6d4fd22b20465ccc0f7354476588e8df7c9ca0e0` | node24 |
| actions/cache | v4 `0057852b` | **v6.1.0** `55cc8345863c7cc4c66a329aec7e433d2d1c52a9` | node24 |
| actions/upload-artifact | v4 `ea165f8d` | **v7.0.2** `cf430e030ddbb5b0abf93d22962f4752f3646cd9` | node24 |
| actions/github-script | v8 `ed597411` | unchanged (v8 already runs node24) | node24 |
| seanmiddleditch/gha-setup-ninja | v5 `96bed6ed` | **removed**: `scripts/install-ninja.py`, Ninja 1.11.1 by digest | (no Node) |
| ilammy/msvc-dev-cmd | v1 `0b201ec7` | **removed**: `scripts/msvc-dev-env.py` | (no Node) |
| KyleMayes/install-llvm-action | v2.0.9 `ebc04262` | **removed**: `scripts/install-clang-tidy.py`, LLVM 20.1.8 by digest | (no Node) |

Release notes read for the inputs this workflow uses:

- checkout v5 (node24), v6 (credentials persisted to a separate file; `persist-credentials: false` still persists
  none, and every checkout keeps it), v7 (refuses to check out a fork's pull request under `pull_request_target` or
  `workflow_run`, neither of which this workflow uses; ESM); v7.0.1 fixes.
- setup-python v6 (node24), v7 (ESM; the `pip-install` input removed, not used here; end-of-life Pythons dropped,
  3.12 is not one); v7.1.0 adds mirror inputs.
- cache v5 (node24), v6 (ESM): `path`, `key`, `restore-keys` and the `cache-hit` output unchanged.
- upload-artifact v5 (preliminary Node 24), v6 (node24 by default), v7 (an optional unzipped single-file upload,
  `archive: false`; zipping stays the default), v7.0.2 retries on HTTP 429. `name`, multi-line `path` and
  `if-no-files-found` unchanged; no workflow downloads an artifact, so no paired download-artifact major.
- The three third-party actions have no Node 24 release: gha-setup-ninja's latest, v6, and its default branch still
  declare `node20`; msvc-dev-cmd's v1.13.0 (2024-01) and its default branch `node20`; install-llvm-action's v2.0.9
  `node20`. Waiting for them would leave the warning; each was a small, checkable step, so each is now the
  repository's own:
  - **Ninja:** `tools/ninja-windows` and `tools/ninja-linux` (1.11.1, the version the action installed by default, so
    the toolchain is unchanged): the release zips with their binary member and its digest; `install-ninja.py` verifies
    both, checks `ninja --version` and appends the directory to `$GITHUB_PATH`. It runs after setup-python (the
    helpers need the pinned Python); on the MSVC lanes vcvars appends Visual Studio's own Ninja at the end of `PATH`,
    so the pinned one stays first (checked on this host).
  - **MSVC:** `msvc-dev-env.py` finds Visual Studio with the x64 C++ tools through vswhere (as `msvc-env.bat` does),
    runs `vcvars64.bat` in a child `cmd.exe` that prints its environment as JSON, and appends every variable vcvars
    added or changed (not `PROMPT`) to `$GITHUB_ENV`, the names in their own case; it fails unless `cl.exe` is on the
    new `PATH` and `INCLUDE`, `LIB` and `VCToolsInstallDir` are set. Here: VS 18 Community, MSVC 14.51.36231, 45
    variables.
  - **LLVM:** `tools/llvm` is now LLVM's `clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz` (939.5 MB; GitHub's
    recorded digest matches the download) with `clang-tidy.exe` and the 294 builtin headers as pinned members;
    `install-clang-tidy.py` (the Linux wheel installer, extended) reads the tar once and installs `bin/clang-tidy.exe`
    and `lib/clang/20/include`, checks the binary's digest, the count and `--version`, and writes `CRD_CI_TIDY` to
    `$GITHUB_ENV`. The archive's `clang-tidy.exe` is byte-identical to this host's `C:/LLVM-20.1.8` one (SHA-256
    `da206027…`), so hosted and local analysis run the same binary. The pinned-inputs gap "LLVM 20.1.8 is acquired by
    a pinned action rather than by digest" is closed.

## Guards and docs

- `check-pins.py`: the two new helpers join `HELPERS`; the workflow must install clang-tidy through
  `install-clang-tidy.py` with "LLVM 20.1.8" named and Ninja through `install-ninja.py` with "Ninja 1.11.1" named (the
  rule that wanted the action's `version: '20.1.8'` input is gone). New tooling tests: the Ninja installer (member,
  digest, version), the MSVC exporter (only changed variables, case-insensitive, `PROMPT` skipped, the delimited form
  for a line break) and the Windows clang-tidy archive (one pass, layout, digest, count, a link refused).
- The license manifest regenerated (19 tools, 5 actions) and its provenance refreshed; pinned-inputs.md and
  repository-layout.md updated.

## Verification (local)

`check-pins.py`, `check-ci-tiers.py`, `test-repository-tools.py` (126 tests), `gen_license_manifest.py --check`, the
pinned actionlint 1.7.12 (`-shellcheck= -pyflakes=`, through WSL) on `ci.yml` and `host-check.yml`: clean. Each
helper ran once here: Ninja 1.11.1 installed and on the path file; the MSVC environment exported; clang-tidy 20.1.8
and 294 headers installed in 41 s, reused on a second run by its stamp, and `tidy-files.py` with that binary analysed
`clocks.cpp` clean (its `<intrin.h>` needs the builtin headers).

Needs CI: the next hosted run shows no Node 20 annotation, every Ninja lane on the pinned 1.11.1, every MSVC lane's
build through the exported environment, and the clang-tidy lane on the installed 20.1.8. The CPM cache key hashes
`cmake/pins.json`, so that run restores the CPM cache through its restore-key prefix.
