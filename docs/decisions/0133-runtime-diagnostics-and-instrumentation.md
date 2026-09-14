# ADR-0133 — Runtime diagnostics and instrumentation

<!-- doc-role: decision -->
> Direction and acceptance contract. Live work: [ROADMAP](../ROADMAP.md#slice-diag.0).

**Status:** scope and immediate-after-REPO.DEV placement authorized by the user on 2026-09-14. The
[implementation contract](../design/runtime-diagnostics.md) defines required outcomes; exact provider/API choices
are resolved with evidence in DIAG.0 and owning leaves. No external vendor, new paid service, new runner or unavailable
hardware acquisition is approved by this ADR. RAH-0 and ADR-0107 remain separate unaccepted gates.

**Extends:** [ADR-0002](0002-logging.md), [ADR-0003](0003-memory-v1.md),
[ADR-0109](0109-ceir-chir-ckir-ownership-and-module-placement.md),
[ADR-0117](0117-compiler-infrastructure-skeleton.md),
[ADR-0120](0120-hot-reload-and-state-migration.md),
[ADR-0129](0129-renderer-ui-editor-delivery-order.md),
[ADR-0130](0130-system-qualification-and-agent-driven-products.md).

## Context

The [research](../research/2026-09-14-diagnostics-and-instrumentation.md) records useful existing sanitizer, profiler,
crash, log and GPU facilities alongside allocator blind spots, reported scheduler races, fragile fatal paths,
unqualified observer/registry lifetimes and incomplete source/capture integration. Building a larger renderer/editor
before a reliable diagnostic foundation would magnify investigation cost. Universal bug detection or effortless
Heisenbug diagnosis cannot be certified; observable bounded detection and reproducibility are the acceptance standard.

## Decisions

1. Insert **DIAG** immediately after REPO.DEV, before REPO.3d and renderer work, in the sole master table. Repair known
   free-list races and qualify the fiber race instrument early, before trusting later shared-state diagnostics.
   Preserve REPO publication gates and every retained renderer/UI/notebook obligation. This is planning authorization,
   not resumption of a stopped implementation loop.
2. Extend existing Cerid modules. Keep engine-facing profiling in crd-perf, ordinary logging in crd-log, allocation
   contracts in memory, lifecycle/scheduling in jobs and source provenance in CEIR/CKIR owners. Lower-layer hooks do
   not depend on UI or an IR executor. Hosts compose collectors/exporters through optional public services.
3. Separate bounded emergency recording from allocating/locking ordinary diagnostics. Prefer a healthy external
   crash collector where supported; the exact maintained backend or narrow OS implementation is chosen after
   licensing, platform and adversarial qualification. Reports must preserve actual failure and write results.
4. Make policies/triggers/experiments authorable through existing asset/configuration/execution services. Native
   adapters remain generic host mechanisms. Freeze validated policy before fatal execution; no parsing, allocation,
   CEIR execution or unsafe callbacks inside the emergency path.
5. Instrument each logical custom allocation and retain explicit lifetime/provenance. Combine sanitizer boundaries,
   structural invariants, optional guards/quarantine/sampling and generation-checked handles. Do not replace Cerid
   allocators with a system allocator or claim raw pointers become lifetime-safe after address reuse.
6. Preserve real causal ordering. Sanitizer annotations cannot invent happens-before merely to silence reports.
   Combine race detection, bounded schedule/weak-memory tests, multicore stress, wait graphs and rare-failure replay.
   Record limitations, including uninstrumented native code and GPU nondeterminism.
7. Correlate CPU/task/GPU/resource/command/program generation and source identity. Keep clocks distinct unless
   calibrated; report loss/uncertainty. Use versioned bounded capture formats and interoperable offline export.
   Future CR-D007 views consume these services rather than building a second profiler/debugger.
8. Default to local, bounded diagnostics with explicit restricted raw evidence. Upload, remote debug, arbitrary
   memory access and fault injection are separate capabilities. A raw dump is not sanitized merely because the
   accompanying manifest is redacted. Agent tools use the same typed authorization and budgets as human controls.
9. Qualify Windows/Linux first, design portable boundaries now, and retain explicit MAC.DIAG/WEB.DIAG release gates.
   No missing hardware or empty tool invocation counts as qualification. Exact platform tuples define support.
10. Use risk-selected local checks and CI for broader evidence. Every claimed detector has failing and valid controls,
    every hot-path mode has measured overhead, and every task updates the owning row/session and affected documents.

## Ownership transfer without lost requirements

DG05's immediate scheduler repair transfers from CORE-USE.2 into DIAG; CORE-USE retains current-renderer integration.
Early allocator/instrumentation/crash/trace mechanisms move forward from MEM-HARD/JOBS-HARD/OPS, while their later
product, peer-performance, distributed and new-platform qualification remains. SANITY-TLSF retains audits of later
consumers. RAH-6.b, LANG and editor inspectors extend the shared mechanisms in their original programmes. The
[contract](../design/runtime-diagnostics.md#existing-programme-ownership-after-diag) gives the exact boundary.

## Consequences and unresolved mechanism choices

The roadmap gains a finite diagnostic foundation before major renderer work. Research citations are guidance, not
installation decisions or performance claims. DIAG.0 resolves tool versions, schema migration and module placement;
child evidence resolves algorithm/provider choices within this authorized scope. Record these decisions here rather
than inventing hidden defaults. A real scope reduction, publication, paid service or changed product authority requires
the applicable user decision. Existing human-only commit/push and no-delegation rules remain unchanged.

## Implementation decisions

No implementation/provider selection or runtime qualification was performed by this planning change. Append numbered,
dated evidence-backed decisions here as the owning slices settle the mechanisms; status remains solely in ROADMAP.
