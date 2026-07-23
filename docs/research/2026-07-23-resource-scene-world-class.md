# Research — 2026-07-23 — the WORLD-CLASS resource + scene system (GEO band, no-gap expansion)

> Round 2 of the geometry-resource research (companion: `2026-07-23-geometry-resource-pipeline.md`), user-directed: "FULL WORLD
> CLASS NO GAP — Cerid makes DAWs, composes video/movies, everything agent-drivable via MCP/CLI; ECS world-class data-driven;
> if a module is missing we BUILD it, not patch it." This doc widens the GEO band from mesh I/O to the complete
> resource-and-scene substrate: ECS integration · dependency-graph asset processing · animation · timeline/sequence (film) ·
> audio (DAW) · the agent surface.

## Question

What does a NO-GAP resource + scene system look like for an engine that is simultaneously a game engine, a DCC (Blender-class),
a CAD/CAM/slicer platform, a film/video compositor, and a DAW substrate — with every operation drivable by AI agents (MCP/CLI)?
What do we already own, what must be BUILT (not patched), and what are the exact slices?

## TL;DR

- **The two hardest pillars are ALREADY BUILT in-tree and are genuinely world-class-architected:** the ECS (`crd-scene`,
  ADR-0050: archetype + sparse-set HYBRID, 16 KB SoA chunks @64-byte alignment, memoized archetype graph, Flecs-class
  relations-as-components, change detection, deferred command buffers, script components) and the cooked scene artifact
  (SCEN, ADR-0055) + **öbek prefab/decomposition system** (ADR-0058, with a planned `obekc` CLI). The math substrates the new
  domains need are also in: **hesap-dsp (v11, DONE) is the DAW's math engine**, hesap-interp feeds animation sampling,
  crd-time/crd-units feed timeline semantics. We integrate and extend — nothing here is patched.
- **The industry validates every Cerid principle:** per-type resources + interchange-only bundles (USD authors / glTF delivers);
  **OTIO** (ASWF) is exactly a "timeline that REFERENCES external media" — our resource philosophy applied to film editorial;
  **CLAP** (MIT, thread-pool multicore, MIDI 2.0 per-note) is the open DAW plugin standard; Bevy-v2/O3DE asset processors
  prove the source→job→product **dependency-graph cook** we must add; **Blender-MCP (50+ agent tools) proves the agent-driven
  DCC pattern** — Cerid's advantage is being agent-native from the substrate, not bolted on.
- **The no-gap GEO band = 11 slices in 4 sub-bands:** import/formats (GEO-1..5, as researched) + resource infrastructure
  (GEO-6 dependency-graph processor) + scene/ECS/animation (GEO-7/8) + the new domains (GEO-9 timeline · GEO-10 audio ·
  GEO-11 agent surface). New modules to BUILD: `crd-audio`, owned JSON/XML parsers, the asset-processor layer, the MCP/CLI
  agent seam (ADR-0081). Video CODECS are explicitly out (patent-encumbered): the professional answer is image-sequence
  (EXR/PNG) + audio masters, muxed at the export edge.

## What we already own (verified in-tree — the honest inventory)

| Pillar | State | Evidence |
|---|---|---|
| **ECS** | ✅ BUILT (world-class architecture) | ADR-0050 Accepted: archetype+sparse-set hybrid; 16 KB chunks, 64-B SoA; `ArchetypeGraph` memoized add/remove edges; `Relation<Tag>` = components-with-payload (Flecs model); change-detect + async-aware indexes; `commands.hpp` deferred buffers; `script_component.hpp`; 8+ test files |
| **Scene artifact** | ✅ designed+built | `scene_resource.hpp`: SCEN crdr container (ADR-0055) — INFO/STRP/CMPS/ETBL chunks, FourCC-identified component tables |
| **Prefabs/decomposition** | ✅ designed | öbek (ADR-0058): canonical-hash prefab roots, `obekc extract` CLI (pillar 14 "Decompose") |
| **Per-type resources** | ✅ BUILT | UUID `ResourceId`s, cooked packs + mounts (newest-wins), sync/async/streamed loads, pin, hot-reload subscriptions |
| **Codecs we own** | ✅ | inflate/deflate (RFC 1950/1951), HDR + OpenEXR codecs (zero 3rd-party doctrine, proven) |
| **Geometry substrate** | ✅ | crd-geometry(+processing): half-edge, subdiv, decimate, remesh, repair, winding, BVH (CPU+GPU) |
| **DSP math (the DAW engine)** | ✅ | hesap-dsp v11 DONE (filters, resampling, spectral); hesap-fft; hesap-interp (animation sampling) |
| **Time/units** | ✅ | crd-time, crd-units (typed units arch — rational time fits here) |
| **Agent-native charter** | ✅ vision doc | `cerid-agent-native-engine.md`: CLI/RPC per operation is a FOUNDING principle (ADR-0081 pending); DAW + cinematic named equal-class domains |
| Dependency-graph asset processor | ❌ BUILD | cook exists but no source→job→product dependency graph / incremental recook daemon |
| JSON / XML parsers | ❌ BUILD | config is units/TOML-oriented; glTF needs JSON, 3MF needs XML — both small, owned |
| Animation/skeleton resources | ❌ BUILD | B8-j skinning math exists; no clip/skeleton resource types |
| Timeline/sequence resource | ❌ BUILD | nothing — the film/video/cinematic pillar's core |
| Audio (module + resources) | ❌ BUILD | no `crd-audio` at all — the DAW pillar's runtime |
| MCP server / engine CLI | ❌ BUILD | vision documented, nothing implemented |
| Video codec I/O | ⛔ deliberately OUT | patent-encumbered (H.264/HEVC); pro pipelines master to image sequences + audio; mux at the edge |

## Recommendation for Cerid — the no-gap GEO band (4 sub-bands, 11 slices)

**Sub-band A — import + formats (GEO-1..5, per the round-1 research, strengthened):** ImportedAsset seam + STL/OBJ/PLY →
MikkTSpace-compatible conditioning → glTF decompose-import (+ our JSON parser, PNG decoder, `SceneResource` link-up) →
scene cook + glTF export round-trip → 3MF manufacturing. Unchanged in intent; GEO-3/4 now explicitly target the EXISTING
SCEN/öbek machinery instead of inventing a scene format (the round-1 doc predates this discovery).

**Sub-band B — resource infrastructure (GEO-6):** the **dependency-graph asset processor** — the Bevy-v2/O3DE lesson: every
cook declares source→job→product dependencies; content-hash incremental recook (only what changed re-cooks — we already
content-hash shaders in D2, extend the discipline to ALL resources); import settings live in per-source `.meta` (ADR-0078
precedent); crash-safe job journal; the whole graph queryable (→ GEO-11 agents). This is what makes "everything is a resource"
*operational* at scale.

**Sub-band C — scene/ECS + animation (GEO-7/8):**
- **GEO-7 — scene↔ECS↔renderer integration:** imported `SceneResource` INSTANTIATES into the ADR-0050 `World` (entities,
  transform hierarchy via `Relation<Parent>`, render components carrying mesh/material `ResourceId`s); öbek prefabs wrap
  imported assets (import → öbek roots → instance many); the render path iterates archetype chunks (SoA-friendly draw
  submission). Uses crd-geometry for spatial queries, crd-hesap for transform math — never sidecar math.
- **GEO-8 — animation resources:** `SkeletonResource` (joint hierarchy + inverse binds) + `AnimClipResource` (keyframe tracks;
  sampled via hesap-interp; cubic/step/linear per glTF), decomposed from glTF skins/animations; feeds B8-j LBS/DQS. The
  same clip machinery is what the GEO-9 timeline automates (one curve/track substrate, two consumers).

**Sub-band D — the new domains (GEO-9/10/11):**
- **GEO-9 — the TIMELINE/SEQUENCE resource (film/video/cinematic):** an OTIO-shaped data model as a first-class resource —
  tracks · clips · transitions · markers · time effects, **referencing** other resources (scene takes, cameras, anim clips,
  audio clips, param-automation curves) and NEVER embedding media — OTIO's own design, which is exactly our per-type
  philosophy. Rational time (frame-rate exact, crd-units); OTIO `.otio` import/export as the interchange edge (NLE
  round-trip: Resolve/Avid/Premiere all speak it). Renders compose to **image sequences (our EXR/PNG) + audio masters**;
  container mux (mp4/mov) is an export-edge integration, never internal. This is simultaneously the game cutscene sequencer
  and the video-composition core — one resource, both products.
- **GEO-10 — AUDIO: the `crd-audio` module + resources (the DAW substrate):** BUILD the missing module — (a)
  `AudioBufferResource` (our own WAV/AIFF/FLAC parsers — RIFF/IFF chunks are trivial; FLAC decode is bounded and fully
  specified) + streamed variants via the existing streaming loader; (b) an **audio-graph resource** (nodes: source, gain,
  mix, filter, send; edges; parameter-automation curves shared with GEO-8/9) — the processing graph IS a resource, exactly
  like a material graph; (c) DSP kernels come from **hesap-dsp** (resampling, filters, FFT) — the engine's own math, per
  the always-reuse rule; (d) `MidiResource` (SMF parse; MIDI 2.0-aware data model — per-note controllers, 32-bit velocity —
  per the CLAP/MIDI-2 direction); (e) the realtime callback rides crd-jobs with a lock-free command queue (the ECS
  commands pattern applied to audio). **CLAP hosting is a named FUTURE phase** (plugin ABI hosting is its own beast) — but
  GEO-10's graph model is CLAP-shaped (thread-pool, sample-accurate events) so hosting slots in without redesign.
- **GEO-11 — the AGENT SURFACE (MCP + CLI, ADR-0081 made real):** every GEO operation reachable headlessly — `ceridc`
  verbs (import · cook · query · instantiate · sequence · render · export) emitting machine-readable JSON reports, and an
  **MCP server exposing the same verbs as tools** (resource graph queries, scene composition, timeline edits, render
  submission). Blender-MCP (50+ tools, any MCP client) proves demand and the pattern; Cerid's edge is that the engine is
  agent-native at the substrate (headless-first, deterministic cooks, UUID-addressable everything) rather than a GUI with a
  socket bolted on. Acceptance bar: an agent, via MCP alone, imports a model, composes a scene, sequences a shot, and
  renders it — zero GUI.

**Ordering:** GEO-1→2→3 (content in) → GEO-7 (scenes instantiate + render — the sandbox showcase) → GEO-6 (processor, once
real cooks exist to graph) → GEO-4/8 → GEO-9/10 (parallel tracks; 10 is a new module) → GEO-11 (wraps everything; its CLI
verbs should GROW with each slice, not arrive at the end — each GEO slice ships its `ceridc` verb). The OFF band interleaves
after GEO-7 (the offline renderer then has real scenes to converge on).

## What we read

- [OpenTimelineIO docs](https://opentimelineio.readthedocs.io/en/latest/) + [OTIO releases](https://github.com/AcademySoftwareFoundation/OpenTimelineIO/releases) + [What is OTIO](https://www.intelligentassistance.com/what-is-open-timeline-io/) — C++ core; clips/tracks/transitions/markers/metadata; **media referenced externally, never embedded**; Resolve/Avid/Premiere support; ASWF-mature. The timeline-resource model AND its interchange edge.
- [CLAP](https://en.wikipedia.org/wiki/CLever_Audio_Plug-in) + [Bitwig on CLAP](https://www.bitwig.com/stories/clap-the-new-audio-plug-in-standard-201/) + [CLAP in 2026](https://producergrid.com/blog/clap-plugin-format-everything-you-need-to-know/) — MIT-licensed, thread-pool multicore host/plugin cooperation, sample-accurate non-destructive modulation, MIDI 2.0 per-note; 15 DAWs / 394 plugins by 2026. The audio-graph + event model to shape GEO-10 around; hosting = future phase.
- [MIDI 2.0](https://en.wikipedia.org/wiki/MIDI_2.0) — per-note controllers, 32-bit resolution, property exchange; the MidiResource data model target (SMF/MIDI-1 as the parse floor).
- [Bevy asset system](https://deepwiki.com/bevyengine/bevy/4-asset-system) + [Bevy asset processing](https://deepwiki.com/bevyengine/bevy/4.2-asset-processing-and-gpu-preparation) + [O3DE asset dependencies](https://www.docs.o3de.org/docs/user-guide/assets/pipeline/asset-dependencies-and-identifiers/) — the modern processor consensus: `.meta`-configured transforms, source/job/product dependency graph, content hashing, crash recovery, hot reload. GEO-6's shape.
- [Blender MCP](https://blendermcp.org/) + [Blender MCP Server listings](https://genai.works/mcp-servers/Blender-MCP-Server) — 50+ agent-orchestrated tools over HTTP/MCP; any MCP client (Claude/ChatGPT/Cursor/local). Proof of the agent-driven DCC market; GEO-11's external benchmark.
- Round-1 doc sources (USD/glTF split, 3MF, last-mile-vs-interchange) — `2026-07-23-geometry-resource-pipeline.md`.
- In-tree: ADR-0050/0055/0058, `crd-scene` headers, `resources/` module, `cerid-agent-native-engine.md`, hesap-dsp/interp, crd-time/units.

## Alternatives considered

- **Adopt OTIO's C++ library instead of owning the timeline model** — rejected for core (3rd-party doctrine; our resource
  must be UUID-native, crdr-cooked, ECS-instantiable). OTIO is the *interchange* format we read/write — same posture as glTF.
- **Adopt a 3rd-party audio engine (miniaudio/JUCE/FMOD)** — rejected: violates the doctrine, and hesap-dsp already IS the
  DSP core no third party would match for our purposes (bit-exact, unit-typed, benched). We build the thin realtime layer
  (device callback + lock-free graph execution) ourselves; the hard math is already ours.
- **Video codecs in-engine** — rejected deliberately: H.264/HEVC are patent-encumbered; AV1 decode is a multi-month codec
  project with no pillar blocked on it. Pro film pipelines master to EXR sequences anyway (our codec). Revisit only if a
  product genuinely needs in-engine video *decode* (e.g. video textures) — flagged, not promised.
- **A separate cutscene sequencer + a separate NLE timeline** — rejected: one rational-time track/clip/curve resource serves
  both (the Unreal-Sequencer-vs-DaVinci split is a product-tier difference, not a data-model one). Divergence would double
  the automation-curve substrate GEO-8/9/10 all share.
- **GUI-first tooling with CLI added later** — rejected by the agent-native charter: every slice ships its headless verb;
  the GUI (post-hesap editor phase) is a client of the same command surface, exactly per the command-layer memory.

## Pitfalls / gotchas

- **Rational time or bust** — a timeline mixing 23.976/24/29.97-drop with float seconds accumulates drift; the time type is
  a rational (num/den ticks, crd-units-typed) from day one, converted only at the edges. OTIO's model is rational for this
  exact reason.
- **Audio is a REALTIME discipline, not a resource discipline** — the callback thread may never allocate, lock, or touch the
  ResourceManager; everything crosses via preloaded buffers + lock-free queues. Design the graph-execution memory story
  (preallocated voice pools) before the first node runs. Our std::counting_semaphore lost-wake scar applies — own the
  primitives.
- **The ECS is chunk-iterating; the renderer must not fight it** — GEO-7's draw submission should walk archetype chunks
  (SoA) rather than per-entity handles, or the world-class ECS is wasted at the renderer boundary.
- **Dependency graphs rot silently** — GEO-6 must make UNDECLARED dependencies structurally impossible (cooks receive only
  declared inputs), else incremental recook produces stale products that "work" until they don't. O3DE's core lesson.
- **MCP surface = attack/chaos surface** — agent verbs must be transactional (the ECS command-buffer pattern), validated,
  and dry-runnable; an agent-driven engine without transactional edits corrupts scenes at machine speed.
- **MIDI 2.0 is a data-model decision even before any device I/O** — if `MidiResource` bakes MIDI-1 7-bit assumptions,
  per-note expression (the CLAP/MIDI-2 world) never fits later. Model 32-bit + per-note now; parse SMF as the floor.

## Next research

1. **NURBS/B-rep CAD kernel** (unchanged from round 1 — its own phase).
2. **CLAP hosting phase research** — plugin ABI, parameter/event routing into our audio graph, sandboxing.
3. **The editor/GUI phase** — how draw/imgui + the command layer + GEO-11's verbs become the Blender-class front end.
4. **Video-texture decode decision** — only if a product pillar demands it (AV1-own vs out-of-scope stays open).
