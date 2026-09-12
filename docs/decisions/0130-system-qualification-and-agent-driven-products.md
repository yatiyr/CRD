# ADR-0130 — System qualification and agent-driven products

<!-- doc-role: decision -->
> Decision record. Live work: [ROADMAP](../ROADMAP.md); rules: [AGENTS](../../AGENTS.md).

**Status:** Proposed for detailed mechanism choices. Product directions below are explicitly user-confirmed
2026-09-12; this file does not accept the separate RAH-0 or full ADR-0107 design gates.
**Extends:** [ADR-0077](0077-multi-domain-expansion-vision.md), [ADR-0081](0081-agent-native-engine-cli.md),
[ADR-0108](0108-ceir-owned-language-stack-supersedes-cpp-only-scripting.md),
[ADR-0129](0129-renderer-ui-editor-delivery-order.md).

## Confirmed product directions

- Preserve the entire retained renderer before native crd-ui/CR-D007, then finish the scientific notebook and retained
  programmes. Windows/Linux first; macOS/web follow. Future geometry/physics implementation is not part of this audit.
- Fully authorable algorithms/assets and a shared typed command surface serve humans, native code and agents.
  CHIR is the owned application/scientific language; C++ native scripts and annotation/reflection/codegen remain first-class.
- **Inference and training qualify together.** Include scoped full training, fine-tuning, distillation, checkpoints,
  evaluation and deployment. Neural-renderer dependencies are qualified before the renderer gate; the full AI product
  closes only with both inference and training, including specialized game/product brains.
- **Separate authoritative contracts:** project host with retained local/offline edits and recovery; authoritative
  multiplayer simulation. Browser collaboration is a first-class later deployment of the same project services.
- Retain arbitrary-skeleton assisted posing, DAW/media, modeling/CAD/CAM/PCB, embedded generation, digital twins,
  aerospace/space/autonomy/sensors and scientific modules. Units, modularity, jobs/allocator integration and matched
  peer-crush evidence apply throughout. The roadmap is the sole live table of slices and findings.

## Proposed mechanisms and required reviews

Adopt the [system qualification contract](../design/system-quality-contract.md) and
[whole-system review](../research/2026-09-12-cerid-whole-system-review.md) as detailed acceptance references.
Each owning slice must resolve its design before implementation; these proposals are not claims that the system exists.

1. A common reflection/property/command/document/transaction model feeds GUI, CLI, MCP and agents; no second scheduler,
   VM, reflection registry or state store is created inside a consumer. Generate schemas from C++ AST-aware tooling
   and CHIR/CEIR producers with stable semantic identities and explicit version migration.
2. Jobs, memory, device work and plugins have explicit owner/lifetime/cancellation contracts. Native ARM64 and browser
   execution require their own implementation and physical/runtime evidence; current x64 assembly is not portable proof.
3. Native trusted plugins and untrusted isolated providers are distinct trust modes. Use maintained cryptographic
   protocols/implementations behind owned interfaces; neither model output nor an asset name grants authority.
4. Evaluate merge algorithms by data type. CRDT text/properties may coexist with authoritative validation/transactions
   for constrained graphs and binary assets. Transport, wire format, key management, storage/consensus and failover
   mechanisms require design and adversarial evidence in NET/COLLAB; no vendor/runtime choice is silently accepted.
5. Qualification pins workload/accuracy/platform/hardware and failure envelopes. DDoS resistance is measured within
   deployment capacity; certification or universal superiority is never inferred from a benchmark or standards citation.

## Consequences

The plan becomes larger because missing acceptance work is explicit. It remains one ordered table; long technical
detail lives in references. Full-victory physics gates supersede weaker historical "within 2×" completion language while
preserving its scenes and correctness tests. Training is no longer excluded by the older inference-only phase contract.
Use domain adapters for interchange; Cerid's runtime ownership remains CEIR/CHIR/CKIR and native public modules.

Before a slice closes, it must leave current docs accurate, a dated session and relevant tests/benchmarks/recipes linked.
Passing documentation validation establishes consistency of the plan, not correctness, security or performance of code.
