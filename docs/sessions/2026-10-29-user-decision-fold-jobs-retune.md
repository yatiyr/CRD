# User decision: HESAP-JOBS-RETUNE folds into SIMD-DISPATCH (2026-10-29)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

## Decision

The user's words, 2026-10-29 about 16:00: "then we fold in HESAP JOBS RETUNE to SIMD-DISPATCH RIGHT?", "diag is
efficiently closed man wtf why you are postponing it?" and "ok then fold hesap jobs retune to simd dispatch and I will
do the elevated runs WHEN SIMD-DISPATCH FULLY FINISHES".

- [HESAP-JOBS-RETUNE](../ROADMAP.md#slice-hesap-jobs-retune) is delivered with
  [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch): its scope is on that row's Left list and the row closes only
  with it. Row 108 keeps its id and anchor, points at SIMD-DISPATCH and closes with it on the same evidence.
- DIAG is no longer a work prerequisite of the retune. What DIAG has left is evidence: DIAG.11b clause (8) is the
  user's elevated capture, run by the user once SIMD-DISPATCH is fully finished (not prepared inside this row), and
  DIAG.11c and DIAG.12 wait on a green complete-tier run. Row 108 now requires SIMD-DISPATCH, so no cycle remains.
- The work is unchanged: every `parallel_for` and task-graph consumer in hesap-dense, hesap-direct, hesap-sched,
  hesap-amg, hesap-iterative and hesap-autodiff re-measured at 1, 4, 8, 16 and 32 threads; a change kept only on a
  saved before/after board with bit-exact or declared-tolerance results; the kept-serial decisions recorded; batched
  fft transforms judged as jobs separately. It runs in the row-close board session, the thread sweep beside the level
  sweep, and any loser is fixed before the row closes.
