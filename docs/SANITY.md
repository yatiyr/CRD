# Cerid sanity checks

<!-- doc-role: rule -->
> Current rule. Current work: [ROADMAP](ROADMAP.md); current rules: [AGENTS](../AGENTS.md).

Read on every entry. This is a checklist, not a status ledger or a claim that the whole engine is defect-free.
The [dated scars and full previous ledger](archive/2026-09-12-orientation-history.md#docs-sanity) remain evidence.
All unresolved work has an owner in [ROADMAP](ROADMAP.md); never maintain a second backlog here.

1. **Root-cause before fixing.** State the mechanism and a discriminating test. A workaround without that mechanism
   does not close the defect. A retried green run alone does not explain an earlier failure.
2. **Verify the actual artifact.** Rebuild changed libraries and every affected executable. Check header/PCH/toolchain
   dependencies. Use scoped CTest plus guards; cached output and a stale sibling executable are not new evidence.
3. **Test boundaries deliberately.** Empty/single/last-element, capacity edges, poisoned storage, invalid handles,
   partial failure and lifecycle transitions matter more than assertion count. Random volume can miss the last block.
4. **Know the instrument's blind spots.** ASan cannot establish intra-pool integrity or absence of uninitialized reads;
   use structural walks/poisoning where appropriate. A non-LTCG build cannot qualify an LTCG-only code path.
5. **Try to refute the performance hypothesis first.** Capture a baseline, profile the limiter, change one mechanism,
   and save the full matched-peer board. Follow the kernel mandate before invoking a hardware limit.
6. **Report the complete board.** Match hardware, accuracy, threads, features and timing boundaries. Keep losses and
   checked unavailable peers visible. Old measurements retain their date/configuration; no invented current grades.
7. **Change method when stalled.** Record ruled-out hypotheses, improve the diagnostic or request an available design
   review. Persist on the objective without repeating an uninformative experiment indefinitely.
8. **Search the engine and its consumers first.** Use capability synonyms and inspect the public API/implementation.
   Reuse or extend the owning module. Search algorithm assembly and consumers, not only type/node declarations.
9. **A measured loss remains open.** Disclosure is evidence, not resolution. Full-victory contracts cannot close on
   ties/losses; broader kernel contracts retain their exact parity/crush bar. Solve the gap without weakening the oracle.
10. **Investigate the failing configuration.** Capture its actual toolchain, generated code and runtime values. A
    config-specific failure may be code, undefined behaviour, environment or a compiler bug; prove which. Fix the
    source mechanism and remove diagnostic scaffolding. Do not lower optimization to make the test pass.
    Normal local iteration uses one primary configuration plus risk-triggered checks; full matrices belong to CI.
11. **Verify the harness before reporting a defect.** Missing ASan DLLs, zero matched tests, unparsed tidy input and
    PowerShell early-closing pipes can manufacture failures or false greens. Slow is not hung: inspect the right
    process, progress and expected duration. Preserve real exit codes; never kill a run merely on a guess.

## Bug workflow

Search the owning ROADMAP row and existing evidence. Record observation, reproduction, expected/actual behaviour,
configuration, harness checks and confidence. An unverified source concern stays **Review**; a reproduced failure
blocks its owning completion gate. Add a stable child ID when work needs separate tracking. Store detailed evidence
in a linked design/session record; change live state only in ROADMAP. Fix → discriminating regression → affected
consumer checks → evidence → close child → close parent only when all obligations pass.

New lessons go in [MEMORY](../MEMORY.md)'s relevant reference or an educative recipe. This file stays short;
historical stories do not grow back into the mandatory reading path.

Cross-domain checks: prove allocation/task/device lifetime, actual target hardware, bounded untrusted input,
unit/frame/time meaning and recovery after interruption. Test authority separately from transport encryption,
collaboration validity separately from convergence, and model task quality separately from throughput. Use the
[quality contract](design/system-quality-contract.md) for applicable gates and complete document/session close-out.
