# CHIR high-level authoring

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

CHIR is Cerid's owned source-language layer. It lowers one-way into CEIR; it is not a separate runtime or VM.
Text and CR-D007 graph documents are two projections of one source model.
[ADR-0128](../decisions/0128-chir-0-language-binding-decisions.md) specifies the shipped CHIR-0 scope.

## What exists

[SourceModel](../../engine/execution/chir/include/crd/chir/node.hpp) carries semantic nodes, stable identity, typed-pin names,
attributes, regions and source locations; presentation layout is separate. The text parser/printer and graph-schema
reader/writer round-trip the committed event-handler example. Both projections lower to the same CEIR module.
Source-derived identity preserves compatible state across reorder/body edits and supports migration/rejection checks.

The [32 close](../sessions/2026-09-06-ceir-32z-band-close.md) proves the five-construct prototype: event handler,
query, parallel update, await and state. Its node kinds also include program and state declarations/updates.
The canonical examples are [text](../../assets/chir/event_handler.chir) and
[graph document](../../assets/chir/event_handler.chirgraph).

## Limits that matter to application authors

CHIR-0 does not have the complete language type system. Modules, generics, ADTs, closures and full application
ownership/event semantics are the intended language scope, not a claim of shipped completeness. The prototype
parallel-for does not produce a CEIR result, and some unbacked consumers/feedback use constants; real multi-handler
state and behaviours require the LANG rows before UI depends on them. See
[lower.cpp](../../engine/execution/chir/src/lower.cpp) for the actual current lowering.

The graph document is a data format, not a finished node-editor widget. CEIR-33 closed C2 domain contracts;
I2D-9 builds real visual editing, diagnostics, undo/redo and preview in CR-D007.

## Intended role

CHIR authors high-level application/UI behaviour; CEIR remains directly authorable and is the canonical executable
form; CKIR carries rendering/compute kernels. Native C++ extensions remain first class, but do not replace authored
algorithm assets with hidden graph builders. The
[execution contract](../design/renderer-ui-execution-contract.md#execution-and-authoring) specifies that boundary.

All new language/UI/editor slices live in [ROADMAP](../ROADMAP.md#master-table). Detailed CHIR-0 grammar, identity
and lowering history remains in the [pre-audit overview](../archive/2026-09-12-superseded-plans.md#system-chir) and
ADR-0128. Inspect current source before extending the prototype.
