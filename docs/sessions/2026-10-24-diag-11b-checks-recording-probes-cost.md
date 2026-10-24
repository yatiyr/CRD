# DIAG.11b (9): what generation checks, recording and probes cost, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [Vulkan GPU-assisted validation](2026-10-24-diag-11b-vulkan-gpu-assisted-validation.md).

## CI read at the start

`gh run list --branch main --limit 15`: every completed push run since 40086275157 was cancelled with no failed job.
The one failed job is still linux-gcc-public-checks (120819184913) of the scheduled run 40075929537, head `420aaf1a`:
`held.fibers[0].high_water == 9U` read 8 (`test_diag_queue_pressure.cpp:290`), which `d96eae30` repairs. Runs
40080021621 and 40097253450 were in progress. (l) zstd 1.5.7 and (o) clang-tidy 20 stay open: this run's only user
message approves no download.

## Change

The runbooks' last three Cost rows said "not boarded": the generation check (`rb-lifetime`), recording (`rb-replay`)
and the boundary probes (`rb-pressure`). None is a mode with a build without it, so each is timed per call against
its own baseline in one binary.

- `tests/bench/test_bench_diag.cpp`, three `[bench-diag]` cases in the existing `crd-bench`: lookups through
  `containers::SlotMap::get` (live and stale) and `scene::SlotMap::is_alive` against a raw index; `plan::run` against
  `run_traced` with `replay.record`'s input recorder at the default and the full event bound, and `encode_record`;
  `call_keeping_fp_environment` around an opaque call, `probe_values`, `jobs::pressure_snapshot` and the lifetime
  ledger. Each case checks its answers first. No new program: `crd-bench` already holds the micro-benchmarks; it now
  links `crd-ceir-cook` and `crd-scene` (`runtime`'s `TEST_DEPENDS`) and reads `assets/diag/overhead_offline.ceir`,
  the harness's offline program.
- A first float-mode benchmark of `read_fp_environment` alone was optimized away under `/GL` (Catch2: "could not
  measure"); a probe brackets an opaque call in real use, so the case measures that shape.
- [Board](../bench/2026-10-24-diag-11b-checks-recording-probes.md); the three Cost cells of
  [the runbooks](../design/diagnostic-runbooks.md) now state the measured costs.

## Results

win-release and linux-gcc-relwithdebinfo (the shipping presets build no benchmarks), two runs each:

- A generation check adds 0.5 to 0.7 ns per lookup over a raw index; an entity check costs about 0.55 ns.
- A traced run costs +35% to +55% over a plain run with 4,096 events kept, +137% to +232% with all 60,004; the encode
  about 45 ns per kept event (78 KB and 1.1 MB records).
- A float-mode boundary costs 14 ns on MSVC and 3.5 ns on GCC; a value probe 1.3 to 2.4 ns per value; a pressure or
  ledger snapshot 10 to 60 ns; a ledger record 15 ns.

## Teeth

Each restored and rebuilt, the three cases green again on win-debug:
- `containers::SlotMap::contains` without its generation compare: the lookup case fails (`count_stale()` 0 == 4096);
- the plan trace's event count not incremented (`trace_safe_point`): the recording case fails (`events_total` 0 ==
  60004).

## Verification

win-debug: crd-bench builds and its three `[bench-diag]` cases pass; the ctest guards. win-release and
linux-gcc-relwithdebinfo: crd-bench builds with no warning and the cases pass (the board's runs). `tidy-files.py` and
`clang-format --dry-run` on the new file, `check-allman-braces.py`, `check-diag-runbooks.py`, `check-master-plan.py`,
`check-repository.py`.

Hosted once pushed: every lane that builds benchmarks builds and links `crd-bench` with the two new libraries and no
warning; `crd-check-diag-runbooks` passes on every Ninja lane.

## Not done in this batch

DIAG.11b's remaining items need the user: (l) the zstd 1.5.7 archive and (o) clang-tidy 20 on the WSL host (each a
download the user approves in their own message), and (8) the elevated recording
(`scripts\elevated-captures.bat`).

Follow-up for an extra batch: none.
