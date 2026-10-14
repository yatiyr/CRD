# DIAG.9c: the elevated TTD recording, 2026-10-14

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.9c](../ROADMAP.md#slice-diag.9c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-9c). Rules: [AGENTS](../../AGENTS.md).
> Preceding: [reverse-debugging routes](2026-10-10-diag-9c-reverse-debug-routes.md).

## The recording

The user ran the `ttd` route of `scripts/reverse-debug-recipes.py` from an Administrator PowerShell, in both modes,
against the win-debug `crd-diag-corruption-specimen`.

Getting a recording took two corrections:
- **The packaged TTD cannot be launched.** The only TTD on the machine was the one inside the WinDbg Store package
  (`C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe\amd64\ttd\TTD.exe`). Launching it
  was refused with WinError 5 even in an elevated shell, because files inside an app package cannot be started
  directly. The recipe called that "an elevated shell is required", which was wrong. The user installed the standalone
  package (`winget install --id Microsoft.TimeTravelDebugging -e`), which adds a `ttd.exe` app alias under
  `%LOCALAPPDATA%\Microsoft\WindowsApps`; the recipe found it on PATH.
- **The backward stop was one instruction late.** The recording ran, and the clean control gave `CLEAN`, but the
  corrupt mode answered `NOT_FOUND: the stop is at line 79, the corrupting store is line 78`. The replay stopped in the
  right function, `crd_diag_corrupt_neighbour+0x53`, but a data breakpoint reports the instruction after the write, so
  a backward run stops just past the store, on the loop's closing line.

## The fixes (`scripts/reverse-debug-recipes.py`)

- The reverse replay steps back one instruction (`g-; t-;`) before it reads the frame, so the frame is the store itself.
  The forward routes are unchanged.
- `--replay-trace <trace.run>` judges a recorded trace again without recording. Only recording needs elevation, so a
  recording can be re-judged from an ordinary shell.
- An elevated refusal of a tool inside an app package names the standalone package to install, instead of asking for
  elevation.

`scripts/test-reverse-debug-recipes.py` now has 35 tests. Three new or extended ones pin these: the step back in the
reverse command (absent from the forward one), the package refusal message (elevated and not), and a replay that
launches no recorder. All three fail against the old recipe. The suite passes on Windows and on the WSL reference host.

## Verdicts

The user's two recordings, re-judged with the fixed recipe from an unelevated shell (`--replay-trace`):

| Mode | Trace | Verdict |
| --- | --- | --- |
| corrupt | 20,971,520 bytes, under the 64 MiB bound | `FOUND: crd_diag_corrupt_neighbour at corruption_specimen.cpp:78`, at `+0x4f`, one instruction before the earlier stop |
| clean | 20,971,520 bytes | `CLEAN: the data breakpoint never triggered` |

The specimen's own uninstrumented run detected the corruption in corrupt mode (`detected word=4`) and none in clean
mode, so there is no observer effect. The recordings and verdict records stay local under
`build/win-debug/ttd-evidence/`; they are evidence of this machine, not committed.

## What remains in DIAG.9c

- rr: an explicit capability limit. WSL2 on this host exposes no hardware performance counters, so rr cannot record
  here. The recipe reports it as `HOST_INELIGIBLE`.
- The hosted run of this change: `crd-diag-reverse-debug-recipes` and the repository step on both runners, with the 35
  tests.
