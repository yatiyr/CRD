# Test scratch files and the host check's mmap probe, 2026-10-15

<!-- doc-role: historical -->
> Dated evidence. Live owners: [DIAG.9a](../ROADMAP.md#slice-diag.9a) (the scratch files) and
> [DIAG.10a](../ROADMAP.md#slice-diag.10a) (the host check). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9a); harness:
> [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [DIAG.10a host check](2026-10-14-diag-10a-host-check.md).

Two follow-ups the close of DIAG.9c to 11a reported. Neither changes engine code or an oracle.

## Scratch files in the working directory

The record and replay suites wrote their scratch programs and run records under the diagnostic file root `.`, that
is, the process's working directory, and removed them only when a case reached its last line. A failing `REQUIRE`
or a crash left them behind; for a run started in the checkout that is the repository root, where
`check-repository.py` rejects them. Before the change, a forced failure (`REQUIRE(false)` right after the first
`spill` of `diag 9a input: a seeded failure records every draw ...`, run from an empty folder) left
`diag9a_in_demo.ceir` in that folder.

The same pattern was in every suite that hands a service a relative root or writes a record by a relative name:

- `crd-ceir-cook-tests`: `test_replay_inputs`, `test_replay_clock`, `test_replay_events`, `test_replay_record`,
  `test_replay_minimize`, `test_host_record`, `test_replay_device` (root `.` and the record files), and the
  `rooted(".")` sections of `test_program_diag`, `test_inspect_diag` and `test_replay_diag` (the `diag8c_*` files);
- `crd-ceir-host-tests`: `test_host_replay_diag` and `test_host_session_record`;
- `crd-ceir-gpu-vulkan-tests` and `crd-ceir-gpu-dx12-tests`: the shared `device_replay_gate.hpp` record file;
- `crd-chir-tests`: the diag 8c cooked blob in `test_chir_reload_provenance`.

The other `crd-ceir-*` suites only name files in memory (provenance paths) or read committed assets by absolute
path. `tests/tools/ceridc` keeps its `in_cwd` device root on purpose: a child process is given it as a relative root,
and ctest gives each of its cases its own directory.

`tests/execution/ceir-cook/scratch_dir.hpp` gives each test process one folder, `<temp>/crd-scratch-<pid>`. The pid
keeps the parallel per-case processes of ctest apart; the folder is made fresh on first use and removed when the
process exits, a failed case's process included. Each suite now gives its diagnostic service `scratch_root()` as
the file root (the request paths stay relative, so every answer and refusal is unchanged, including the
`../escape.ceir` and `../out.crpl` argument refusals), and routes its own reads, writes and removals through
`scratch_path()` or `resolve()` (absolute paths, the committed assets, pass through). The paths live in fixed
buffers, so the folder allocates nothing that outlives a call. `crd-chir-tests` now links `crd-platform`, declared in
the module's `TEST_DEPENDS`; `crd-ceir-host-tests` links it as well.

## The host check's vm.mmap_rnd_bits

The host check summary showed `vm.mmap_rnd_bits unknown` on the hosted runners. On Ubuntu the file is readable by
root only, and `sysctl -n` reads the same file, so it fails the same way. On this machine's WSL guest, as an
ordinary user: `cat /proc/sys/vm/mmap_rnd_bits` gives Permission denied, `sysctl -n vm.mmap_rnd_bits` exits 1 with
"permission denied on key", and `sudo -n true` exits 1 ("a password is required"). A hosted runner's user has
password-free sudo.

`scripts/fuzz-host-check.py` now tries, in order: the file; `sysctl -n vm.mmap_rnd_bits`; `sudo -n sysctl -n
vm.mmap_rnd_bits` (`-n` never prompts; each command has a 10 s timeout). A route counts only when it exits 0 with a
number on stdout, so an error message never becomes the value; `unknown` remains only when all three fail. On this
WSL guest the probe answers `unknown`, as expected; the next hosted host-check run is where the value shows.
`test_host_check_reads_mmap_rnd_bits_through_each_route` drives every route without root: the file, `sysctl`,
`sudo` after a refusal, a missing `sysctl`, all refused, and a non-numeric answer with a timeout.

## Rows the close step named

REPO.3 (Partial, prerequisite REPO.3d Open) and MED-1 (Partial, prerequisite MED-REVIEW Open) are classified as
their rows say: their remaining work is local and waits on an open prerequisite, not on a hosted run. DIAG.11b is
the first unfinished row and `context.md` already points at it. No row changed state.

## Verification

- Teeth, `crd-ceir-cook-tests` win-debug, the case above run from an empty folder: before the change the forced
  failure left `diag9a_in_demo.ceir` there; after it, the same forced failure left nothing in the folder and no
  `crd-scratch-*` folder under the temp directory; a hard exit (`std::_Exit(3)` after the same `spill`) left the
  folder empty and `diag9a_in_demo.ceir` only in `%TEMP%/crd-scratch-<pid>` (removed by hand). The source was
  restored and the lane rebuilt; the whole suite then passed with nothing left in either place.
- win-debug, win-shipping and win-clang-cl-shipping: `crd-ceir-cook-tests` (115 cases), `crd-ceir-host-tests` (55),
  `crd-chir-tests` (30), `crd-ceir-gpu-vulkan-tests` (60) and `crd-ceir-gpu-dx12-tests` (47) build and pass, each
  run from an empty folder that stays empty, with no `crd-scratch-*` folder left. 132 diag 8c to 10b cases pass under
  `ctest -j 16` on win-debug.
- `linux-gcc-debug` (WSL): the cook, host, chir and Vulkan suites build and pass from an empty folder that stays
  empty, with no `/tmp/crd-scratch-*` left.
- The tooling test fails when a route's output is taken without its exit code (tried, then restored);
  `test-repository-tools.py` passes.
- Strict tidy (LLVM 20.1.8) is clean on all 15 changed C++ files; the new header is clang-format clean, and the
  edited files keep their earlier clang-format counts (hand alignment and include groups, unchanged). The Allman
  check, the `crd-no-*`/`crd-check*` guards, `check-master-plan.py` and `check-repository.py` pass.
