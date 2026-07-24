# 2026-07-24 — GEO-9: the TIMELINE resource (D-007 row 74)

**Slice**: the TIMELINE/SEQUENCE resource — OTIO-shaped editorial data in RATIONAL time, `.otio` interchange,
deterministic evaluation, EXR-sequence render, param automation. Closed in one session, every gate green.

## What shipped

1. **`crd/time/rational_time.hpp/.cpp`** (crd-time — the leaf home the research named): `RationalRate` +
   `RationalTime` + `TimeRange`. Same-rate math is pure i64; cross-rate compare/add run through portable
   two-limb 128-bit intermediates (never a rounded rescale). `rescaled_to` states its rounding;
   `rescales_exactly` asks first. The f64 edges are LOSSLESS: SMPTE-family snap → continued fractions
   (round-trip-verified) → exact binary fraction; subframe values fold into the rate. SMPTE timecode includes
   29.97/59.94 **drop-frame** (SMPTE 12M), gated by an **exhaustive 2,589,408-frame broadcast-day round-trip**.
   Suite: **time 5,179,006 asserts / 66 cases**.

2. **`engine/timeline` (crd-timeline, NEW)** — the 'TIML' resource: tracks · clips · transitions · markers ·
   time-effects · **automation curves** (the GEO-8 deferred param-track home; hesap-interp keyframe semantics —
   the ONE curve engine — with **rational-exact segment selection**). Media referenced by ResourceId slot +
   URI always, never embedded. ONE validator gates build AND load. Zero-init POD records ⇒ byte-identical
   rebuilds (the GEO-6 determinism contract).

3. **Deterministic eval** (`timeline_eval`): OTIO's sequential track model; transitions OVERLAY the cut — both
   neighbors active inside the window, weights sum to 1, media handles extend past trims; LinearTimeWarp /
   FreezeFrame warp media time through the exact-rational scalar edge; cross-rate audio (samples) exact.

4. **The EXR render driver** (`timeline_render`): per-frame evaluate → weight-mix in linear light → OUR EXR
   codec; media through an injected resolver (the renderer band's future seam). **THE GATE**: 2 takes +
   centered dissolve + audio track + automated camera param → 96-frame EXR sequence; decoded pixels equal the
   hand-computed mix **bit-exact**; two renders **byte-identical**. Suite: timeline 348/6.

5. **The `.otio` edge** (asset-io, the glTF posture): reader (Clip.1/.2 with active-key, External/Missing/
   ImageSequence references, Gap, Transition, Marker.2 at item/track/stack level, LinearTimeWarp/FreezeFrame;
   unknown effects/media COUNTED in the diag; nested Stacks REFUSE with the schema named) + writer (%.17g —
   export∘import is a FIXED POINT, second generation byte-identical).
   **REFERENCE-NLE ORACLE, BOTH DIRECTIONS (OpenTimelineIO 0.18.1 via pip — the ffmpeg/mikktspace test-oracle
   precedent):** their `reference_nle.otio` (generator: scratchpad `gen_reference_otio.py`; checked in at
   `tests/asset-io/data/otio/`) imports exactly in the hermetic gate; our export (dumped via the `CRD_OTIO_DUMP`
   hook — the 3MF-dump precedent) loads in the official lib, structure agrees, re-serializes clean.
   Suite: asset-io 856/49.

6. **The cook edge**: `.otio` handler v1 (CookIO-only reads; clips without source_range RESOLVE their trim
   from the media's available_range — the artifact is always eval-ready; an untimeable clip refuses).
   Cook → load → evaluate gated; second cook byte-identical. Suite: cooker 313/28.

7. **Live**: the sandbox camera is timeline-driven — a real TimelineResource (20 s looping crane shot, two
   CubicHermite automation tracks) sampled per frame in rational time on a 24000 ticks/s query grid against
   24 fps keys: the cross-rate evaluator visibly at work. Smoke PASS.

## Numbers

time 5,179,006/66 · timeline 348/6 · asset-io 856/49 · cooker 313/28 · anim 1,559/5 · scene-render 58/5 ·
hesap-interp 605/35 (last three: zero regression). 21 files tidy-clean.

## Deferred with homes

- Audio-master render → GEO-10 (crd-audio).
- MediaKind::Resource mapping (timeline → cooked engine media) → lands with its consumers; the format
  carries the ResourceId slot from day one.
- Automation in `.otio` → OTIO has no curve schema; automation is native-only (documented in otio.hpp).
- Nested-Stack compound clips → refused loudly today; a future row when compound workflows are needed.

## Lessons

- The exhaustive gate was cheap and decisive: the full drop-frame day (2.59M frames) runs in ~1 s and pins
  the SMPTE algorithm far harder than landmark cases alone.
- OTIO's f64s are exactly recoverable: the SMPTE snap + continued-fraction + binary-fraction ladder made the
  import edge genuinely lossless — "rational or bust" held without a single tolerance.
