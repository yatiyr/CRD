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
- **Research.** Three serial research parts (engine/GPU/rendering plus computational geometry and navigation; domains
  beyond games; how agents and people build with Cerid), each a dated docs/research document whose proposed rows the
  user approves before the roadmap changes.
