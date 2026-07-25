# `docs/design/` — per-slice IMPLEMENTATION SPECS

> **What this directory is for.** A design doc here is the **implementation contract for ONE slice**: what
> already exists (so it is reused, not rebuilt), what is genuinely missing, the sequenced increments, the gate
> for each, the named risks, and the explicit non-goals. It sits between the **row** (the one-line contract in
> a phase/detour table) and the **session log** (what actually happened).
>
> **How to find the doc for a slice you have been asked to implement:** the slice's row in its phase or detour
> table links it BY PATH. Start at `context.md` → the active phase/detour → the row → this directory. The index
> below is the reverse lookup.

## Conventions

- **One file per slice**, named `<slice-id>-<slug>.md` (e.g. `ren-3-lighting-shadow-pipeline.md`).
- **The row is the pointer.** Whenever a spec is written, the slice's row MUST be edited to link it — a design
  doc that nothing points at is invisible to the next agent, which is the exact failure this index exists to
  prevent (found 2026-07-25: `docs/design/` was not referenced from `docs/README.md` at all, so the canonical
  reading order never reached it).
- **A REUSE AUDIT is mandatory before the increments.** Grep the engine first (SANITY #8) and state, per gap,
  whether it is *wiring* or *new work*, with file:line evidence. The REN-3 spec's sky section is the worked
  example: what read as "new work" turned out to be four LUT kernels already dispatching oracle-green on
  Vulkan, which shrank the increment substantially.
- **Every increment carries its gate**, and the whole slice carries its acceptance criteria + named
  non-goals, so an omission is a decision rather than drift.
- **Specs are living until their slice closes**, then they are historical — the session log supersedes them as
  the record of what shipped. Do not retro-edit a closed spec; write the divergence in the session log.

## Index

| slice | doc | status |
|---|---|---|
| REN-2 | [ren-2-rtt-and-material-textures.md](ren-2-rtt-and-material-textures.md) — RTT transients + sampled material textures | ✅ closed 2026-07-25 |
| REN-3 | [ren-3-lighting-shadow-pipeline.md](ren-3-lighting-shadow-pipeline.md) — lighting · shadow · procedural sky · full AA, **sandbox-visible** | ⬜ next |
| REN-3.1 | [ren-3-1-depth-rtt-transients.md](ren-3-1-depth-rtt-transients.md) — RTT **depth** transients + `draw_storage_depth_only` (the shadow-map substrate) | ✅ closed 2026-07-25 (gated both backends + bench) |
| **REN-36** | [ren-36-authorable-frame-graph.md](ren-36-authorable-frame-graph.md) — ⭐ **render passes, pipelines and whole rendering ARCHITECTURES as authorable assets, API-agnostic** (user-declared MUST) | ⬜ next, peer of REN-3.2 |
| REN band | [ren-band-reuse-audit.md](ren-band-reuse-audit.md) — pass-1 audit of all 35 rows. **Read its method warning**: pass 1 mostly re-derived what the rows already say and was wrong twice; the rows carry their own reuse notes | 📋 reference |
| hesap-fft | [hesap_fft_generated_codelets.md](hesap_fft_generated_codelets.md) — the generated FFT codelet scheme | ✅ shipped |
