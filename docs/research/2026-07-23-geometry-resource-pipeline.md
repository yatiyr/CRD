# Research — 2026-07-23 — the geometry-resource pipeline (import → decompose → author → cook)

> The design research for Cerid's mesh/scene I/O + resource layer — the foundation the offline renderer (OFF band), the
> real-time renderer, CAD/CAM/slicing, and physics all consume. Produces the **GEO band** slices (D-007, interleaved before/with
> the OFF band). User-directed 2026-07-23.

## Question

Cerid is not a game engine that "takes meshes and textures" — it is a Blender/CAD/CAM/slicer-class system where **mesh
manipulation is our own job** and **every resource type must have a reason**. How do we build the import/resource layer so that
(a) we own every parser, (b) bundle formats (glTF/USD/FBX) are *interchange only* — imported by DECOMPOSING into our native
per-type resources and exported by re-bundling — and (c) the same layer serves rendering, CAD/CAM/slicing, and the future
node/DCC tooling?

## TL;DR

- **The philosophy is already the architecture.** Cerid's resource system is per-type by construction: UUID `ResourceId`s,
  separately-cooked `MeshResource` / `MaterialTemplate` / `TextureResource` / shader (crdr) resources, mounted cooked packs,
  streaming + hot-reload. glTF-style bundling is foreign to it — so import = **decompose**, export = **re-bundle**. This matches
  the industry's own split: **USD = authoring/interchange, glTF = delivery** (AOUSD↔Khronos formal liaison says exactly this).
- **We own more of the substrate than expected.** Full mesh-processing library (half-edge, subdivide, decimate, remesh, repair,
  winding-number, BVH), a cooked native mesh format (48-byte vertex, SI-units at cook), our own **inflate (RFC 1950/1951)** +
  HDR/EXR codecs. The gaps: **external-format parsers (none), a JSON parser (none), a SceneResource (none), tangent/normal
  generation on import (none), NURBS/B-rep (none — its own future phase)**.
- **Format ladder, all parsers our own:** Wave 1 = STL/OBJ/PLY (mesh-only, trivially ownable) → Wave 2 = **glTF 2.0** (the
  delivery standard; JSON+GLB; decompose into scene/mesh/material/texture) → Wave 3 = **3MF** (the manufacturing/slicer-native
  format: OPC/ZIP + XML, slice + beam-lattice extensions — our inflate already reads it) → Wave 4 = USD subset (production
  interchange, long-term) → **STEP AP242 rides the future NURBS/B-rep kernel phase** (parsing STEP without a B-rep kernel is
  pointless).

## Recommendation for Cerid

**The pipeline (one shape for every format):**

```
file bytes ──(OUR parser)──▶ ImportedAsset (in-memory intermediate model)
                                  │ decompose
      ┌─────────────┬─────────────┼──────────────┬───────────────┐
      ▼             ▼             ▼              ▼               ▼
  SceneResource  TriangleMesh  material params  image bytes   (curves/anim later)
  (entities,      │ condition   │ AUTHOR onto   │ OUR decoders
   transforms,    │ validate/   │ OUR OpenPBR   │ (HDR/EXR ✅,
   mesh+material  │ repair,     │ MaterialTemplate + CKIR      PNG via our
   refs, lights,  │ normals,    │ surface graph (B5/B6/B8-k)   inflate, JPEG
   cameras)       │ TANGENTS    │ — never "translate shaders"  later)
      │           ▼             ▼              ▼
      └──────── cook ──▶ per-type cooked resources (UUID'd, packed, mounted)
                          └──(export = re-bundle from native)──▶ glTF / 3MF / …
```

Key decisions:

1. **Bundles are interchange, never storage.** A glTF import produces N native resources (1 scene + meshes + materials +
   textures), each with its own UUID, cook, and lifecycle. Export re-bundles from native. We never keep a glTF "asset" around.
2. **Materials are AUTHORED, not translated.** glTF metallic-roughness (and later USD Preview Surface) parameters map onto OUR
   OpenPBR surface (B5 slab superset ⇒ lossless mapping) instantiated through the existing `MaterialTemplate` + B8-k cook seam.
   The *shaders* are always ours — CKIR-authored, per-pass lowered. No foreign shader ever enters the system.
3. **The intermediate `ImportedAsset` model is the seam.** Parsers only fill it; decomposition/cook only reads it. Adding a
   format = adding a parser, nothing downstream changes. It also hosts import-time mesh conditioning (validate → repair →
   weighted normals → **MikkTSpace-compatible tangents** — required since STL/OBJ/PLY lack tangents and our cooked vertex
   carries them; MikkTSpace-compatible because every DCC bakes normal maps against it).
4. **Parser toolkit we own once:** a JSON parser (glTF; ~small, well-specified), an XML parser (3MF), a ZIP/OPC reader (3MF —
   our `inflate` already handles the streams), binary readers (STL/PLY/GLB). All allocator-aware, span-based, no std containers.
5. **NURBS/B-rep is its own future phase** (the CAD kernel — exact parametric surfaces/solids, STEP AP242, trimming, Booleans on
   B-reps). It is deliberately NOT a GEO slice: it is OpenCASCADE-scale and deserves its own research + ADR. The GEO band's
   `ImportedAsset` + SceneResource are designed so B-rep bodies slot in later as another first-class geometry resource type.

**The GEO band (new D-007 slices, interleaved before/with the OFF band):**

- **GEO-1 — import substrate + Wave-1 formats:** the `ImportedAsset` model + parser toolkit + **STL (binary+ASCII) · OBJ+MTL ·
  PLY (ascii+binary_little/big)** parsers → `TriangleMesh` → validate/repair hook → cook → `MeshResource`. Gate: real files
  import, condition, cook, load, and draw in the sandbox.
- **GEO-2 — mesh conditioning:** weighted vertex normals + **our MikkTSpace-compatible tangent generation** + degenerate/weld
  cleanup on import (reusing crd-geometry validate/repair). Gate: normal-mapped render correct vs a DCC-baked reference.
- **GEO-3 — glTF 2.0 decompose-import:** our JSON parser + GLB reader; scene graph → `SceneResource` (NEW per-type resource:
  entities, transform hierarchy, mesh/material refs, lights, cameras); metallic-roughness → OpenPBR `MaterialTemplate`;
  textures → our decoders (**PNG via our inflate now; baseline JPEG its own sub-slice** — honest sizing: Huffman+DCT, ~a
  session, our hesap DCT helps). Gate: a Khronos sample (e.g. DamagedHelmet) imports into N native resources and renders.
- **GEO-4 — SceneResource cook + export:** the native scene format (cooked, UUID-ref'd) + the first EXPORTER (re-bundle → glTF)
  proving the round-trip: import → native → edit → export.
- **GEO-5 — 3MF (manufacturing):** OPC/ZIP + XML core spec import/export (+ slice/beam-lattice extension awareness) — the
  slicer/3D-printing pillar's native format, and the first CAM-facing consumer of the geometry substrate.

**Sequencing vs the OFF band:** GEO-1 → GEO-2 → GEO-3 land **before OFF-1** (the offline renderer needs real content to render,
and the sandbox showcase is the forcing function); GEO-4/5 interleave with OFF work. NURBS/B-rep phase-doc research runs as a
separate thread.

## What we read

- [OpenUSD Reaches Industrial Scale (2026)](https://www.techtimes.com/articles/319575/20260702/openusd-reaches-industrial-scale-pixar-3d-standard-eyes-iso-ratification.htm) — USD Core Spec 1.0 → ISO path; industrial members joining AOUSD. USD is the production-interchange pole.
- [glTF or OpenUSD?](https://www.linkedin.com/pulse/gltf-openusd-which-best-3d-file-format-use-jed-fisher-s24ce) + AOUSD↔Khronos liaison coverage — the consensus split: **USD authors, glTF delivers**. Cerid's decompose-on-import matches this: we author natively, bundle only at the edges.
- [Nick Porcino — 3D File Formats: Last Mile and Interchange](https://nickporcino.com/papers/lastmile-v2/) — the interchange-vs-last-mile framing that names exactly why bundles must not be the internal representation.
- [3MF Beam Lattice extension](https://www.tctmagazine.com/3mf-releases-beam-lattice-extension-3d-printing/) + [3MF at LoC](https://www.loc.gov/preservation/digital/formats/fdd/fdd000557.shtml) + [CAD Interop 3MF](https://www.cadinterop.com/en/formats/mesh/3mf.html) — 3MF = OPC/ZIP + XML, with slice + beam-lattice extensions; the modern manufacturing format (STL's successor), directly relevant to the slicer-analyzer ambition.
- Engine ground truth (read in-tree): `crd-geometry`(+`-mesh-processing`) substrate; `engine/resources` (ResourceManager, cooked packs, mounts, streaming, **deflate.hpp: our RFC 1950/1951 inflate**, our HDR/EXR codecs); `engine/renderer` resources (`mesh_resource.hpp` 48-byte cooked vertex + SI-unit cook per ADR-0078, `material_template.hpp` CookedParameter/ShaderOptions/pass-keyed shaders, `frame_graph.hpp`); `docs/research/cerid-geometry.md` (the substrate charter).

## Alternatives considered

- **Adopt a third-party importer (assimp / tinygltf / cgltf)** — rejected: violates the zero-third-party doctrine (own HDR codec
  precedent), imports foreign allocation/error models, and the parsers are the *easy* part once `ImportedAsset` exists. Owning
  them is what makes CAD/slicer-grade robustness (T-junctions, non-manifold, degenerate faces) OUR fixable problem.
- **Make glTF the native scene format** — rejected on the user's principle: bundling erases per-type identity (no per-resource
  UUID/cook/streaming/hot-reload), couples us to a delivery format's limits, and breaks the authoring story (materials must be
  authored onto OUR templates, not stored as glTF JSON).
- **USD as the native format** — rejected for core: USD is a composition *engine* (layers/overrides/variants) far beyond what a
  cooked runtime should carry; the right relationship is a future USD *importer/exporter* (Wave 4), possibly a USD-inspired
  layering story in the editor later.
- **Start with STEP/NURBS now** — rejected: STEP without a B-rep kernel is unparseable-in-spirit (you can read the file and do
  nothing with it). The kernel is its own phase; GEO keeps the door open via the geometry-resource-type seam.
- **FBX** — deprioritized: proprietary/reverse-engineered; glTF covers the DCC bridge today and USD covers it tomorrow.

## Pitfalls / gotchas

- **Tangent spaces are a compatibility contract, not a convenience** — normal maps are baked against MikkTSpace in every DCC;
  a "reasonable" tangent generator that isn't Mikk-compatible shades subtly wrong forever. Implement the exact algorithm
  (order-independent accumulation, splitting by UV mirroring) and gate against a DCC-baked reference render.
- **OBJ/STL are dirtier than their specs** — negative indices, missing normals, mixed line endings, non-manifold soup, Z-up vs
  Y-up, units (STL has none — CAD exports are frequently in mm). The import path must run validate→repair and expose
  unit/axis policy; ADR-0078's SI-unit cook is the anchor.
- **glTF's tightness is deceptive** — sparse accessors, interleaved buffer views, non-power-of-two strides, KHR extensions
  (transmission, ior, emissive_strength map DIRECTLY onto our OpenPBR slab — support them from day one or imports silently
  flatten), sRGB-vs-linear per texture slot (baseColor sRGB, normal/metallicRoughness linear — a classic silent bug).
- **PNG before JPEG** — PNG is our inflate + defilter (small); baseline JPEG (Huffman + IDCT + chroma upsampling) is real but
  bounded work. Don't let JPEG block GEO-3: PNG + our HDR/EXR covers most sample assets; JPEG lands as its own sub-slice.
- **Winding/orientation on export to manufacturing** — slicers assume outward-facing, watertight, positive-volume solids; the
  3MF path must run winding-number/manifold checks at export (we own the tools already — wire them in).
- **The scar precedent:** every import feature needs its own oracle-grade gate (golden files, round-trip bit-stability of
  cook→load, reference renders) — parsers are exactly where silent corruption enters an engine.

## Next research

1. **NURBS/B-rep CAD kernel** — the dedicated phase research (curves/surfaces, trimming, Euler ops, Booleans, STEP AP242,
   tessellation into the render mesh path). The big one; separate doc.
2. **USD import subset design** (Wave 4) — which prims/schemas (Mesh, Xform, Material/UsdPreviewSurface, Scope), usda-text
   first vs usdc binary.
3. **Animation/skinning resources** — glTF animations/skins → our own clip + skeleton resource types (feeds B8-j skinning).
4. **Slicer analysis pillar** — 3MF slice extension, layer preview rendering (rides the offline/real-time renderer), printability
   analysis (overhangs via our SDF/winding tools).
