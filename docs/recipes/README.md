# docs/recipes — educative build-recipes

A **recipe** is a standalone teaching document for a technique, algorithm, or system we have studied and built.
Its job: **a human or an agent who reads it once understands the thing completely** — the physics/maths, every
parameter, the full assembly, the papers it comes from, and the traps that cost us time. Not a changelog, not an
API reference — a *lesson*.

This folder exists because we learn from many papers, and that knowledge is worthless if it lives only in code
comments and one engineer's head. A recipe is where a subject is taught end to end.

## When to write one

**Whenever we study something and build it** — an algorithm, a rendering technique, a numerical method, a data
structure, a device feature. If we spent real effort understanding a paper (or several) and turning it into
working code, that understanding gets a recipe. This is a standing rule; see `AGENTS.md`.

## What every recipe MUST contain, in this order

1. **PARAMETERS FIRST.** A complete table of every knob — name, meaning, units, sensible default, and the range
   over which it behaves. A reader should be able to drive the system from this table alone before reading a word
   of theory. What each parameter *physically is*, not just what it does.
2. **What it is / why it exists** — the problem, in plain language, and why the naive approach fails.
3. **The physics / maths** — the actual model, derived or cited, enough that the equations in the code are
   understood, not copied. Cite the papers precisely (author, year, section/equation).
4. **The full assembly** — every stage, in order, with the data that flows between them. A reader could rebuild
   the system from this section.
5. **The traps** — every scar. The bugs that cost us time, WHY they happened, and how to recognise the symptom.
   This is the most valuable section and the one only we can write.
6. **Measured numbers** — link the `docs/bench/` board; never quote perf from memory.
7. **Where the code lives** — the files, so the recipe and the implementation stay connected.

## Naming

`YYYY-MM-DD-<subject>.md` (e.g. `2026-07-21-hair-offline-film.md`). A subject may have more than one recipe when
the offline and real-time forms are genuinely different systems (as hair does).

## Index

- **2026-07-21-hair-offline-film.md** — physically-based path-traced hair, film/offline quality. The Marschner /
  Chiang fibre BCSDF, linear-swept-sphere geometry, multi-bounce GI, and the full renderer. The reference the
  real-time tier is measured against.
- **2026-07-21-hair-realtime.md** — the same hair in real time (games): the levers that turn the 194 ms/sample
  film path into a 29 ms/frame real-time path, plus temporal accumulation + denoising and the raster tier.
