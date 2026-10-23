# DIAG.11b clause (8): the external sampling capture's attributions, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11b](../ROADMAP.md#slice-diag.11b). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11b). Rules: [AGENTS](../../AGENTS.md). Earlier read:
> [2026-10-23](2026-10-23-diag-11b-realistic-disabled-cost.md#external-sampling-capture-clause-8-read).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failure without a fix on main. 40022015924,
40020783027 and 40018724265 were cancelled or replaced while pending (no job). 40006451399 was running, every finished
job green; not waited on. The red nightlies 39923363836 and 39746853196 are the three repairs of 2026-10-23.

## Why the capture misattributed

`build/diag-overhead-wpr/20261019T080356Z` (part 1 of the elevated captures) was re-read with short `xperf -a dumper
-range` slices of single processes. Four defects in `scripts/sample-overhead-wpr.py`, each shown on the trace:

1. `-add_inline` appends every function inlined anywhere into a frame's function (sorted), not the inline chain at the
   sampled address. `main` and `OfflineWorkload::iterate` list `MemoryStats` and recorder methods, so nearly every
   sample of every arm, the compiled-out binary's included, had a class. With the list stripped, no out-of-line frame
   of the off or the compiled-out arm is diagnostic.
2. Any diagnostic ancestor claimed a sample: the CEIR bridge's `profiled_run` around the program and the jobs
   adapter around a job took the workload code they call. Now the leaf-most frame of the program's own image that is
   not `std::` decides (a system lock or the standard clock counts for its caller). The bridge's class also missed its
   hooks `adapter_pre`/`adapter_post`.
3. PIDs reused in the session: the counter took a sample by PID alone. `vcpkgsrv.exe` (Visual Studio's) ran under
   offline compiled-out's PID 81696 (2,619 samples, a third of that cell) and under jobs full's 14936 (1,514); `git`,
   `conhost`, `bash` and `timeout` under others. A PID now counts only under the harness image of its run, and two
   recorded runs sharing a PID are unresolved.
4. A rebuild had overwritten the profiling binary's PDB; the re-read worked only because `C:\SymCache` kept the
   symcache. `record` now copies both PDBs under `<run>/symbols/`, which `analyze` searches first. Reusing the
   existing script was enough; nothing new was added.

Cases with teeth in `test-sample-overhead-wpr.py` (each fix removed, the suite failed, restored): the inlinee list,
the bridge ancestor and its clock read, the reused PID in the trace and in the manifest, the kept PDB.

## The verdicts, read

Re-analysis of the same trace with the repaired script (no elevation, 552 s):

| Cell | Measured | External | Reading | Owner |
| --- | ---: | ---: | --- | --- |
| alloc off over compiled-out | +4.6% | 0.0% | The statistics gate is force-inlined, so a frame sampler leaves it in its host function. The exact count explains it: 8,192 gates × 0.50 ns = 4.1 µs, 4.8% of the 85.7 µs compiled-out median | clause (1), met on realistic frames |
| jobs off over compiled-out | +3.5% | 0.0% | No diagnostic sample in either arm. The median is the main thread's wall frame; the samples are CPU of eight workers (3.35 to 3.55 ms per 0.37 to 0.42 ms frame) | clause (4) |
| jobs basic over off | -5.1% | 0.0% | as above | clause (4) |
| jobs full over off | +93.1% | +145.1% | 59.2% diagnostic: the jobs adapter's callbacks (41,987) and the recorder (7,980). In a 0.1 s slice, 359 of the 398 callback samples are system frames under `cb_on_job_begin`/`cb_on_job_end`: the global `g_tokens_mutex` of `park_token`/`take_token`. CPU against wall again | clause (4); the mutex is clause (6) |
| offline off over compiled-out | -13.0% | 0.0% | No diagnostic sample in either arm; compiled-out spent 0.436 CPU ms per frame in the same functions where off spent 0.376 | clause (1a), the layout floor |
| offline basic over off | +1.8% | +0.2% | AGREES | none |
| offline full over off | +540.2% | +396.1% | 79.8% in the bridge (its hooks and the clock they read). The program's own frames took 28% more samples than in off: the executor calls the hooks around every op | clause (6), per-op timing |
| alloc basic, alloc full | unresolved | unresolved | The frames covered 84% of the process (basic's setup takes 0.35 s) | next recording |

The `record` sizes now give at least 3.2 s of frames per process (alloc 40,000, jobs and offline 8,000). That alloc
cell needs a new recording, and the recording is the user's elevated step. `scripts\elevated-captures.bat` now refuses
the sampling step too, with exit 7, while Visual Studio runs. A stand-in copy checks `explorer.exe` in place of
`devenv.exe`: `sampling`, `build-insights` and `all` all exit 7, and an unknown step exits 64. The real script,
unelevated, reaches `wpr -start` and exits 3.

## Not done in this batch

- (l) zstd 1.5.7 and (o) clang-tidy 20 are downloads. Each waits for the user's own approval in the conversation.
- The user's re-recording: `scripts\elevated-captures.bat sampling` from an elevated prompt, with Visual Studio closed.
