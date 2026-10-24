# DIAG.12: one ELF module walk for crd-core and crd-perf, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.12](../ROADMAP.md#slice-diag.12). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-12),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [the crash record's import route](2026-10-24-diag-12-crash-record-import.md).

## CI read at the start

`gh run list --branch main --limit 15`: every completed run (40092153460 to 40110939655) was read job by job; none
has a failed job. 40103197370 (head `6b214076`) is in progress and 40112350438 (head `d6a54ce6`) pending; neither was
touched.

## Change

The previous batch's follow-up, the first duplicated service of DIAG.12: `crash.cpp`'s `extract_build_id` and
`module_enumeration.cpp`'s `note_build_id` were the same PT_NOTE walk, and both callbacks computed the PT_LOAD span.

- `crd::crash::read_elf_image` (declared in `crash.hpp` under `__linux__`, defined in `crash.cpp`) returns one loaded
  object's base, PT_LOAD span and GNU build-id. The header forward-declares `dl_phdr_info`, so no consumer of
  `crash.hpp` reaches `<link.h>`. crd-perf already links crd-core, so no new file or dependency was needed.
- It keeps the stronger bound of the two (the wrapped-add check on a note's size). Each caller keeps its own policy:
  the crash record keeps a module with an empty span and names the executable `""`; the live enumeration drops it and
  names the executable from `/proc/self/exe`. The record format is unchanged.
- Both old walks and both span loops are deleted; `module_enumeration.cpp` no longer includes `<elf.h>`.

## Teeth

Each break was built, run, restored, rebuilt and rerun on linux-gcc-debug (`build/diag12e/teeth.py`, not committed).

| Break in `read_elf_image` | Result |
|---|---|
| size always 0 | 7 cases fail: both `live modules` cases, the frame-hitch bundle, and the use-after-free, wait, crash and fail-on-purpose application cases |
| build-id length always 0 | 6 cases fail, among them `crash record (linux): carries the exe ELF build-id matching the on-disk binary`, both `live modules` cases and the crash case |

## Verification

- win-debug: `crd-core-tests`, `crd-perf-tests` built; the ctest guards and the `live modules` and `diagnostic host`
  cases pass (37 tests).
- linux-gcc-debug and linux-clang-tsan (`wsl-lane.sh build`/`test`): the four touched targets built `-Werror`; the 40
  `crash record|crash capture|live modules|diagnostic host` CTests pass on each.
- `tidy-files.py` clean on the three files (the Linux branch is not analysed until clang-tidy 20 is on the WSL host,
  DIAG.11b (o), the user's). `clang-format --dry-run`: the added lines' findings are the house hand alignment only.
  `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: the same cases pass on every Linux lane.

## Remaining in DIAG.12

The application's authored-program error and GPU hazard, faulty and repaired; privacy across every class; removal of
the remaining obsolete scaffolding and duplicated services; the re-read of every child and DG01-DG18 against final
source and published CI.
