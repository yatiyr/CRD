# Portable strict LLVM-20 analysis for the scoped check

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.DEV.3b.3](../ROADMAP.md#slice-repo.dev.3b.3); contract:
> [strict analysis](../design/developer-workflow.md#strict-analysis). Rules: [AGENTS](../../AGENTS.md).
> Preceding batch: [third-party register](2026-09-13-third-party-defect-register.md).
> Follow-up: the four sampled headers' findings are repaired in [strict-gate header repairs](2026-09-13-strict-gate-header-repairs.md).

## User direction

With the register gate's first hosted run still in flight, the user asked for the REPO slices to be finished in order:
"meanwhile I want you to fully finish REPO slices! Drive them till the end". REPO.DEV.3b.3 was the first truly Open
row: the scoped check ran the Windows-only PowerShell helper and, on any other host, raised "Portable strict-analysis
execution is not yet qualified; changed C++ remains ungated". Same session loop, same rules; no commit or push by the agent.

## Design

- **One helper, every host.** `scripts/cerid_dev/tidy.py` behind `scripts/tidy-files.py`; `tidy-files.ps1` is now a
  thin Windows wrapper with the same positional files, `-BuildDir` and `-ExportFixesDirectory`.
- **Tool resolution refuses substitutes.** `resolve_clang_tidy` (in `environment.py`, shared with `doctor`) probes
  `--version` and accepts only LLVM major 20: an explicit `--clang-tidy`/`CRD_CLANG_TIDY` is the only candidate,
  otherwise the pinned `C:/LLVM-20.1.8` install, then `clang-tidy-20` and `clang-tidy` on PATH. Every rejection is
  recorded with its reported version; nothing found is `unavailable`, exit 99, every file `ungated`.
- **Real flags, derived.** The configured build's `compile_commands.json` is mirrored with only precompiled-header
  inputs stripped (MSVC `/Yu`, `/Fp`, `/FI cmake_pch`; GCC `-include cmake_pch.hxx`, `-Winvalid-pch`; quoted and
  `arguments` forms handled). The database's compiler decides the `--extra-arg` set: MSVC restates `/EHsc` and the
  cache's `CRD_SIMD_MSVC_ARCH_FLAG` (read, never a literal); GCC adds `-Wno-unknown-warning-option` because the
  database's `-Werror` would otherwise turn GCC-only warning names into parse failures.
- **Headers through an owning unit.** A header, or a source the configuration does not compile, is analysed as the
  main file of a translation unit of its owning target (matched through `CMakeFiles/<target>.dir/` in the entry's
  output, from the plan's owners; an owner with a unit under the header's own module directory outranks the plan's
  alphabetical order, so `observer.hpp` takes `crd-jobs`'s flags rather than the first of 130 consumers), then of a
  sibling under the same module directory; the synthesized entry keeps the
  sibling's flags with `-x c++` (GCC) or `/TP` (MSVC) and records `source: owner:<target>` or `module:<dir>`. No
  candidate is `ungated` with the reason. Header units add `-Wno-pragma-once-outside-header` only: under the real
  database flags (`/WX`, `-Werror`) the main-file `#pragma once` diagnostic is an error by construction, while the
  previous `--`-driven helper never showed it because it ran without `-Werror` and the `Checks` glob hid clang warnings.
- **Classification and evidence.** Unchanged from the PowerShell gate: `file not found` is ungated, `warning:`/`error:`
  lines are issues, a nonzero exit without diagnostics is ungated, a missing file is missing. The JSON summary carries
  tool, version, database and mirror digests, the cache ISA flag, per-file status/source/target/exit/diagnostics and
  timing. `check` runs the helper through its supervisor, stores the summary as `record['tidy']` in the sealed evidence
  and maps it with `tidy_outcome`: findings → `failed` (1); unavailable, ungated or missing → `incomplete` (3); no
  usable summary → `instrument_failure`. A pass requires every file `clean` with exit 0 on both sides.

## Verification

- `scripts/test-dev-workflow.py`: five `TidyTests` cases with a clang-tidy stand-in that reports a configured LLVM
  version, logs every argv and answers each file with canned output. They cover PCH-only stripping on the real MSVC and
  GCC command lines, compiler-family and cache-derived extra arguments, owner-then-module header resolution with
  synthesized entries, clean/issues/ungated/missing classification through `analyse` with the mirrored database and
  `-p` scratch, the `tidy_outcome` mapping, and the refusal paths (an 18.1.3 stand-in never analyses anything; an
  explicit wrong tool does not fall through; no candidate names what was searched; a missing database raises).
  **54 of 54** pass on Windows (48 existing plus the five and the Git-budget case) and **52 of 54** on Linux in WSL
  with the two existing Windows-only cases explicitly skipped, so the stub launcher, PATH discovery and the POSIX
  path substitution are proven on both hosted repository platforms.
- Windows, real LLVM 20.1.8 against `build/win-debug` (1,814 entries): `test_work_smoke_dx12.cpp` and
  `test_scene_render.cpp` from their database entries and `frame_graph.hpp` through `module:engine/gpu/gpu-context`
  all **clean**; a mistyped path is **MISSING** and the run exits nonzero as `incomplete`. `doctor` reports the
  resolved tool and version.
- Linux (WSL Ubuntu 24.04, no clang-tidy installed), same helper against `build/linux-gcc-debug`: **unavailable,
  exit 99**, both files `UNGATED` with "clang-tidy 20 is unavailable: no candidate (clang-tidy-20, clang-tidy)". The
  mapping of that result to an `incomplete` check is proven by the `tidy_outcome` tests and the Windows end-to-end run
  below, not by a Linux `check` with a C++ path, which waits for clang-tidy 20 in WSL and then covers both at once.
  `build/linux-gcc-debug` was reconfigured through the
  `linux-gcc-debug` preset (configure only, 4 min 55 s on the 9p mount) so the Linux database matches the current tree.
- Scoped Windows `check` with the tidy step, `dev.py check --build build/win-debug --path
  tests/execution/ceir-gpu-dx12/test_work_smoke_dx12.cpp --jobs 2`. The first run (`20260913T192410-be5918e12629`)
  passed every phase, a guarded configure of the stale model, build, discovery, **65/65** CEIR DX12 CTests, both
  guards and the tidy phase, and then refused the verdict as `instrument_failure` because tracked documents were
  edited during the run: the source-identity guard doing its job. The clean rerun (`20260913T193005-2b902409a928`)
  **passed**: 65 selected, reported, executed and passed, zero skipped or disabled; tidy phase exit 0 with the summary
  sealed as `tidy.json` (LLVM 20.1.8, 1,814 database entries, `/arch:AVX2` read from the cache, one file clean from
  its database entry in 14.8 s); evidence integrity verified over 25 artifacts. Earlier the same command refused to
  start while a registered native generation (`cmake.exe` for `build/win-vs-debug`) held the structure lock, which is
  the coordination contract, not a defect.
- Header sample under the real database flags: nineteen headers across assets, execution, foundation, geometry, GPU,
  media, numerics and rendering, chosen for namespace-scope constants and static helpers. Fifteen clean; four carry
  pre-existing findings (`ckir_lighting.hpp`, `smolyak.hpp`, `bessel.hpp`, `heavy_tail.hpp`: naming,
  missing-std-forward, incorrect-roundings, nested conditional, confusable identifiers) that the previous helper
  reports line for line. No `clang-diagnostic-*` main-file artifact other than the handled `#pragma once` appeared,
  so no further header relaxation was added. The four files are untouched here: their findings predate this work
  and the hosted strict lane reports none of them.
- Linux `doctor` on `build/linux-gcc-debug` reports `clang_tidy_20: null` with the same reason, g++ and Ninja resolved,
  seven eligible presets and no issues: the missing strict tool is reported, not treated as an environment failure,
  because a documentation or non-C++ check on that host is still valid.

## Linux positive arm

The user approved installing clang-tidy 20 in WSL later the same evening (no `sudo` was needed or used; nothing on
the Windows side changed).

- **Tool.** The GitHub release CDN throttled the pinned `LLVM-20.1.8-Linux-X64.tar.xz` to about 85 KB/s per
  connection, so it was fetched as 24 parallel byte ranges on the Windows side (2,021,269,412 bytes, SHA-256
  `1ead36b3dfcb774b57be530df42bec70ab2d239fbce9889447c7a29a4ddc1ae6`), verified with `gh attestation verify --repo
  llvm/llvm-project` against the release's sigstore bundle, extracted into the WSL home (11 GB) and probed: `LLVM
  version 20.1.8`, every shared library resolves. While it downloaded, the PyPI `clang-tidy` 20.1.0 wheel (the only
  major-20 wheel published, a third-party build) ran the first passes from a virtual environment; both tools are
  recorded so they can be compared.
- **Sample.** 27 files against `build/linux-gcc-debug` (1,774 entries): the nineteen-header sample, `worker_pool.cpp`
  and `observer.hpp`, five sources with POSIX branches the Windows gate never analyses (`platform.cpp`,
  `fiber_pool.cpp`, `semaphore.cpp`, `topology.cpp`, `profile_resolver.cpp`), `test_scene_render.cpp` and
  `test_jobs_adapter.cpp`.
- **First finding, GCC flags.** With the wheel: 22 clean, 5 with clang's own compiler diagnostics under GCC's flags
  in two shared headers, not tidy checks (`-Wpedantic` nested-anon-types in `containers/string.hpp`; clang-only
  `-Wsign-conversion` in `scene/component.hpp`), both of which GCC compiles clean under `-Werror` on every Linux
  lane. Rule: GCC is the compiler of record for a GCC database, so the GNU family adds `-Wno-error`; every tidy
  check stays warnings-as-errors and every hard error still fails. Rerun: 27 of 27 clean.
- **Second finding, header scope.** With the pinned 20.1.8: 23 of 27 files reported 456 macro-usage and
  macro-to-enum hits, all in shared headers (`crd/core/platform.hpp`, the generated `build_config.hpp`), while the
  wheel and Windows reported none. A/B on one unit: the pinned tool reports 51 with the PCH include stripped and 0
  with it present; Windows reports 0 with the repository regex and 53 with `--header-filter=.*`. The repository's
  `HeaderFilterRegex` is spelled with `/` separators, which never match Windows paths, so the hosted strict lane
  and the Windows helper have only ever enforced main-file diagnostics; on Linux the regex matches and 20.1.8 also
  diagnoses macros in headers reached from the main file. Rule: the gate passes `--header-filter=` on every host,
  so every lane enforces the same main-file contract and a header is gated by naming it. Under that scope: Windows
  sample 24 of 24 clean, unchanged; Linux pinned **27 of 27 clean** (2 min 16 s); Linux wheel 27 of 27 clean.
- **Third finding, Git budget.** The first Linux `check` ended `instrument_failure` because `git diff HEAD` exceeded
  the frontend's 60 s Git budget on the 9p mount (98 s cold, 3.7 s and 2.4 s afterwards). `plan` and `check` now
  take `--git-timeout` (default 60 s, recorded in the evidence), validated like the other budgets; one new
  `test-dev-workflow.py` case (54 on Windows, 52 applicable on Linux).
- **Linux `check`.** `dev.py check --build build/linux-gcc-debug --path tests/foundation/jobs/test_counter.cpp
  --target crd-jobs-tests --jobs 2 --git-timeout 600`, the jobs test executable never built on Linux before. With
  the wheel (`20260913T220855-8b22fc8ab314`): **passed**, build 70 s, fresh discovery, **120 of 120** CTests, both
  guards (the validator guard 57 s over 9p), tidy 6.2 s, integrity verified over 25 artifacts, source identity
  unchanged. A pinned-tool rerun started while two gate passes were still hammering the mount failed honestly:
  `crd-master-plan` exceeded its 60 s CTest budget, 119 of 120 passed, nothing skipped, exit 8 retained
  (`20260913T222233-268b13ebcb0c`), and again alone: the validator measures 57 to 64 s over 9p against a 60 s
  CTest ceiling. The three repository guards now carry a 300 s ceiling (`tests/CMakeLists.txt`; seconds on a native
  disk, a ceiling against hangs), and the pinned-tool check then **passed** (`20260913T225337-c86cb200c2f7`):
  120 of 120, both guards, tidy phase with `LLVM version 20.1.8`, integrity verified; details in the
  [frontend qualification](2026-09-13-frontend-qualification.md).

## State

REPO.DEV.3b.3 is Needs CI: the Windows arm, the Linux refusal arm, the pinned-tool Linux arm and the `check`
mapping are proven locally, and the hosted repository jobs of the next push carry the unit tests. There is no hosted
Linux strict lane; that belongs to REPO.DEV.5. Three sibling quadrature headers keep their recorded findings.
