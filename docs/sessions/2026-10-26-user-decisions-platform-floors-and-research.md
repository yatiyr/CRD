# User decisions: platform floors, budgets, renderer redesign and research (2026-10-26)

<!-- doc-role: session -->
> Session record; live work and status belong only to [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md).

The user's standing order: always the proper, complete, world-class implementation; elegant, non-bloated code that
misses no feature. Recommendations lead with the gold-standard option, never the simpler one.

## Decisions

- **x64 CPU floor.** SSE2 (the x86-64 baseline) stays a supported floor for wide hardware. This replaces DIAG.11b
  follow-up (r)'s "one AVX2 level for the whole program". The new [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch)
  row carries runtime CPU dispatch; it runs after the C2 flight-recorder work and before the elevated sampling re-record.
- **Flight recorder.** Option C2 from the [ring decision brief](2026-10-26-flight-recorder-per-thread-rings-decision.md):
  one global sequence, per-thread rings; ADR-0133's order and window stay as written.
- **GPU floor.** Fallbacks for older GPUs are the plan; no modern-only floor. RAH rows declare capability tiers with
  authored fallbacks.
- **Budgets.** Measured from peers: beat the best peer; parity with the best is accepted only when no lever remains,
  with that evidence recorded.
- **Peers.** Approved: Tracy, Eigen, cuBLAS, MKL or OpenBLAS, Mitsuba 3 or pbrt-v4, Filament, Jolt, PhysX. Each actual
  download is still approved one by one.
- **Native Linux display.** Open gate; no dual boot now.
- **RAH-0** ([brief](2026-10-26-rah-0-review-brief.md)): all nine breaking changes accepted with nothing missed;
  re-cook every asset from source under new format versions; RAH-1 typed attachments first; the refreshed no-loss
  census on today's tree is the gate.
- **QUAL.0** ([brief](2026-10-26-qual-0-review-brief.md)): the points above answer its CPU, GPU, Linux-display, peer and
  budget questions.
- **Runtime changeability.** Frame graphs, pipelines, pass processing, shaders, materials and every GPU program stay
  fully changeable at runtime (authored in CEIR/CHIR/CKIR, swapped without restart or engine rebuild); this is why CHIR
  exists and RAH-7 owns live replacement.
- **Numerics and REPL.** crd-hesap covers what MATLAB's core does (CHIR-SCI, NOTEBOOK); an interactive REPL follows the
  UI system ([ADR-0065](../decisions/0065-hesap-numerical-substrate.md) section 9 names `crd-hesap-repl`).
- **Capability ownership.** Every engine capability is implemented in its one correct module; crd-hesap is the
  computational base for every solver and matrix need in any system, so nothing is duplicated. The
  [OWN.1](../ROADMAP.md#slice-own.1) registry and audit and the [OWN.2](../ROADMAP.md#slice-own.2) guard carry it, after
  HESAP-JOBS-RETUNE. Mods, plugins, user projects and CHIR/CEIR scripts stay free to implement anything; the guard covers
  engine sources only.
- **Research adopted.** After reading the three research parts' proposals (about 110 new rows and 50 amendments), the
  user approved all of them on 2026-10-26. [RESEARCH-ADOPT](../ROADMAP.md#slice-research-adopt) adds them to the
  table in full; on 2026-10-27 the user moved it ahead of SIMD-DISPATCH so it runs next.
- **Part 4.** On 2026-10-27 the user approved every proposal of the
  [networking and crash-pipeline research](../research/2026-10-27-networking-and-crash-pipeline-2026-comparison.md)
  and a legal and regulatory register (COMPLY); RESEARCH-ADOPT reopened to add them in full.
  Standing order: completely secured against malicious
  attacks, robust, and obeying every applicable law, with no shortcuts. Turkish law is a first-class
  jurisdiction in COMPLY (KVKK, the Cybersecurity Law, internet, communications, e-commerce and consumer
  law), never omitted.
- **Part 5 and publishing.** On 2026-10-27 the user approved every proposal of the
  [publishing research](../research/2026-10-27-publishing-packaging-and-app-management-2026-comparison.md)
  and decided: the publisher is the user's Turkish company; shipping waits until everything is implemented,
  working and qualified, then the user opens accounts, certificates and registrations; a Cerid-based store
  (its own name later) serves Turkish customers first, then nearby countries, planned as a STORE row family.
- **Hardware-fault resilience.** This host's i9-14900K still misexecutes rarely after the BIOS update; users will
  have such machines too. Cerid must work gracefully on faulty hardware, with every behaviour explained:
  [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1) to .4 (integrity and retry, hardware identity and
  classification, self-checks and degradation, documentation), right after SIMD-DISPATCH.
- **Research.** Three serial research parts (engine/GPU/rendering plus computational geometry and navigation; domains
  beyond games; how agents and people build with Cerid), each a dated docs/research document whose proposed rows the
  user approves before the roadmap changes.
