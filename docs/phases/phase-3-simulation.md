# Phase 3 — Simulation foundation + visual effects

**Status:** ⏳ planned

Cerid's move beyond pure rendering: scene graph, physics, animation, font,
audio — then the full visual effects stack that makes the engine presentable.
The rendering slices (3.5–3.9) are what drive the sandbox and editor demos.

---

## Slice order rationale

Scene/ECS (3.0) ships before Physics (3.1). Physics needs transforms to sync
into. All later slices (animation, audio, culling) share the same dependency on
the scene graph.

The visual effects stack (3.5–3.9) is ordered from "least infrastructure"
to "most infrastructure": surface shading → atmosphere → post-processing →
GPU simulation → global illumination. Each slice adds frame graph passes and
optionally new resource types; none requires structural changes to the ones before.

---

## Slices

### 3.0 — Scene / ECS

| Slice | Topic                        | Notes                                                         |
| :---: | ---------------------------- | ------------------------------------------------------------- |
| 3.0a  | `crd-scene` graph            | hybrid hierarchy + entity/component; UI nodes share the tree  |
| 3.0b  | SoA component storage        | cache-friendly iteration; hierarchical traversal kept separate |
| 3.0c  | Scene serialization          | TOML authoring → cooked binary; asset_cooker integration      |
| 3.0d  | First real scene             | camera (FPS + orbit) + meshes via cooked assets + skybox      |

### 3.1 — Physics

| Slice | Topic                        | Notes                                                         |
| :---: | ---------------------------- | ------------------------------------------------------------- |
| 3.1a  | `crd-physics` interface      | rigid body, collider, constraint, world, query API; backend-neutral |
| 3.1b  | `crd-physics-physx` backend  | PhysX 5.x as the first backend                               |
| 3.1c  | Physics ↔ scene integration  | transform sync; fixed-step option; deterministic mode flag    |

### 3.2 — Animation

| Slice | Topic                        | Notes                                                         |
| :---: | ---------------------------- | ------------------------------------------------------------- |
| 3.2a  | Skeletal animation           | skeletons, clips, sampling, GPU skinning path                 |
| 3.2b  | Blend trees                  | 1D / 2D blends, additive, layer masks                        |
| 3.2c  | Inverse kinematics           | two-bone IK, FABRIK, target/pole constraints                  |
| 3.2d  | Cinematic timeline           | track-based authoring, deterministic playback, asset binding  |

### 3.3 — Font rendering

Separate phase file: `docs/phases/phase-3.3-font.md`. MTSDF atlas, FreeType +
msdfgen cooker, HarfBuzz complex shaping (Arabic RTL, CJK, ligatures), billboard
text renderer, `DynamicFontAtlas`, extruded 3D text mesh.

### 3.4 — Audio

| Slice | Topic                        | Notes                                                         |
| :---: | ---------------------------- | ------------------------------------------------------------- |
| 3.4a  | `crd-audio` graph            | low-latency mix graph, DAW-grade jitter targets               |
| 3.4b  | Spatialization               | HRTF / panning, occlusion hooks                               |
| 3.4c  | DAW-facing extensions        | sample-accurate scheduling, plugin host scaffold              |

---

## 3.5 — PBR shading + lighting + NPR

The core visual quality of the engine. Every effect here is a shading model or
a lighting contribution that operates in the Forward pass (or a thin set of
additional passes). No deferred G-buffer required.

### 3.5a — HDR pipeline foundation

Switch `ForwardRenderPath` from `B8G8R8A8Unorm` to `R16G16B16A16Sfloat` (scene-
linear HDR). Add a tone-map pass before the swapchain blit. Prerequisite for
everything else in this section — bloom, IBL, emissive, all require values > 1.0
to be meaningful.

- ACES filmic tone map (Academy standard, parameterised by exposure bias)
- AgX tone map as an alternative (better hue preservation in high-chroma)
- Exposure control (manual EV / auto-exposure from luminance histogram)

### 3.5b — PBR: Cook-Torrance GGX

Metallic-roughness workflow (glTF material model):

- **BRDF**: Cook-Torrance specular (GGX distribution, Smith geometry, Fresnel-Schlick).
  Lambertian diffuse for the non-metallic contribution.
- **Textures**: albedo, normal map (tangent-space), roughness/metallic (packed channels),
  ambient occlusion (baked, separate from SSAO), emissive.
- **SurfaceData GLSL contract**: `crd_evaluate_surface(VertexAttrs, inout SurfaceData)` —
  the interface between vertex shading and the light loop. Defined once; all
  shading models fill the same struct.

### 3.5c — Image-Based Lighting (IBL)

- **HDRI import**: `.hdr` / `.exr` → equirectangular TextureResource → cube cross cooker
- **Diffuse irradiance map**: spherical harmonics projection (SH L2, 9 coefficients) or
  full irradiance cubemap; baked per-HDRI at cook time
- **Specular prefiltered env map**: mip-chain at varying roughness (split-sum approximation)
- **BRDF LUT**: 2D Smith split-sum texture baked once, reused across all materials
- **Skybox pass**: screen-space full-screen draw using sky-view LUT or the raw cubemap as fallback

### 3.5d — Punctual lights + shadows

- **Light types**: directional, point, spot (cone angle + penumbra blend)
- **Physical attenuation**: inverse-square with smooth falloff window function
- **Cascaded Shadow Maps (CSM)**: 4 cascades, PCF soft filtering (3×3 Poisson disk),
  cascade blend at boundaries; shadow map updated once per frame per cascade
- **Percentage-Closer Soft Shadows (PCSS)**: blocker search + penumbra width estimation;
  soft shadows scale with light radius and receiver distance
- **Contact shadows**: short screen-space ray for close occluders (cheap,
  captures shadow at foot of a character from a far shadow map)
- **Translucent shadow maps**: tinted shadows through coloured glass / fabric
- **Point light shadows**: cubemap shadow map (6 faces); lower resolution than CSM

### 3.5e — Advanced shading models

These are `ShaderOptionDecl`-gated variants inside the forward shader — they
compile out on materials that don't need them.

| Model | Description | Use case |
|-------|-------------|----------|
| **Clear coat** | Second specular lobe (GGX) + base attenuation | Car paint, lacquered wood, wet surfaces |
| **Anisotropic specular** | Tangent-space elongated GGX (Ashikhmin–Shirley) | Brushed metal, vinyl records, CDs, hair |
| **Cloth / velvet** | Ashikhmin-Premoze inverted specular lobe + sheen | Velvet, denim, microfibre |
| **Iridescence** | Thin-film interference phase shift (analytical) | Soap bubbles, oil slicks, beetle shells, aurora |
| **Transmission** | Refraction via IBL LOD + thickness attenuation (Beer-Lambert) | Glass, ice, water, thin fabric, coloured gems |

### 3.5f — Subsurface Scattering (SSS)

Light penetrates translucent surfaces (skin, wax, marble, jade, leaves).

**Pre-integrated SSS** (Phase 3.5f — first ship):
- Precomputed LUT keyed on `(NdotL, curvature, diffusion_profile_index)`
- 4 diffusion profiles (skin / wax / marble / leaf) baked as a texture atlas
- Scattering tint (RGB) + radius per profile; authored as material parameters
- Single extra texture lookup in the forward shader; negligible cost
- Good match for skin at standard distances; no extra passes required

**Screen-space SSS** (Phase 3.5f — second ship):
- Jimenez 2015 separable blur: 6-tap horizontal + 6-tap vertical in SSS irradiance buffer
- Stencil marks SSS pixels in the forward pass; blur passes are stencil-masked
- 3 blur radii (RGB channels of the diffusion profile); weighted sum
- Better than pre-integrated for close-ups (skin pores, leaf veins, wax candles)
- 2 extra frame graph passes; ~0.3ms on RTX 3070 at 1440p

### 3.5g — Emissive meshes

- `ParameterType::Float4` emissive colour × `ParameterType::Float` emissive intensity
- Values > 1.0 are physically meaningful in the HDR pipeline (e.g. a LED strip at 100 nits)
- Emissive pixels fed into the bloom pass via a threshold extract
- Emissive mesh lights (treat mesh surface as an area light source): Phase 3.5h

### 3.5h — Area lights

- **Rectangular area lights** using Linearly Transformed Cosines (LTC, Heitz 2016)
  — two LTC LUT textures (64×64), pre-baked; GGX fit for specular, Lambertian for diffuse
- **Disk + sphere area lights**: analytic formula for diffuse; LTC for specular
- **Tube lights**: two-end-point linear area light (hair lighting, neon tubes)
- **Emissive mesh lights**: mesh surface area sampled as polygonal area light; uses LTC
  solid-angle integration over a polygon. Approximate but convincing for convex shapes.

### 3.5i — Toon / NPR shading

Non-photorealistic rendering as a first-class `ShaderOptionDecl` variant:

- **Cel shading**: replace smooth diffuse with a stepped ramp texture lookup; configurable
  step count (2-shade, 3-shade, smooth). Ramp is a 1D TextureResource parameter.
- **Toon specular**: Blinn-Phong with step threshold → cartoon specular highlight
- **Outline pass**: inverted-hull second pass (front-face culled, vertex push along normal);
  outline colour and width are material parameters. Registered as `PassType::Outline`
  (extend PassType enum). Works cleanly with the multi-pass PASS chunk.
- **Rim lighting**: Fresnel-based rim highlight; colour + intensity as material params
- **Hatching**: screen-space hatch lines via precomputed tone-dependent hatch atlas;
  replaces or blends with the diffuse ramp
- **Watercolour / painterly**: paper texture overlay + edge bleed post-pass (Phase 3.7i)

---

## 3.6 — Atmosphere + volumetrics

Physically-based sky, fog, and volumetric lighting. These are compute-heavy passes
added to the frame graph before or alongside the main colour pass. They do not touch
the surface material system at all.

### 3.6a — Sky atmosphere (Hillaire 2020)

Bruneton-style precomputed atmospheric scattering, production-ready variant by
Sébastien Hillaire ("A Scalable and Production Ready Sky and Atmosphere Rendering
Technique", EGSR 2020). Used in Unreal Engine 5's SkyAtmosphere component.

**LUT pipeline** (compute passes, runs once on atmosphere param change):

| LUT | Resolution | Content |
|-----|------------|---------|
| Transmittance LUT | 256×64 | Optical depth integral; atmosphere opacity for a ray |
| Multi-scatter LUT | 32×32 | Second-order scattering approximation (fast infinite bounce) |
| Sky-view LUT | 192×108 | Full sky dome at ground level; per-frame (direction varies) |
| Aerial perspective LUT | 32×32×32 | Froxel volume; RGB inscatter + A transmittance along frustum rays |

**Sky rendering**: full-screen quad samples the sky-view LUT. Sub-millisecond.

**Sun**: physically-sized disk (0.53° angular diameter), limb-darkening; drives
the directional light colour and intensity from the transmittance LUT at sun
elevation angle.

**Moon + stars**: star field from a Mollweide-projected atlas; moon phase disc;
both use the transmittance LUT for atmospheric colouring at twilight.

**Time-of-day**: sky system exposes `sun_direction`, `turbidity`, `mie_scattering`
as runtime parameters; LUTs invalidated on change.

### 3.6b — Volumetric fog

Froxel-based (frustum-aligned voxel grid) volumetric fog and participating media.

- Grid: 160×90×64 voxels along the view frustum (depth distribution: exponential)
- **Injection pass** (compute): writes density + emission + scattering albedo per froxel.
  Point + spot lights contribute local density modulation.
- **Scattering pass** (compute): ray-marches through the grid front-to-back,
  accumulates in-scattered light using the Phase Function (Henyey-Greenstein, `g` parameter).
  The aerial perspective LUT from 3.6a is sampled as the background inscatter.
- **Apply pass**: blended onto the colour buffer before tone mapping. Screen-space reprojection
  with temporal smoothing; motion vectors handle camera movement.
- **Fog parameters**: global density, height falloff, extinction colour, scattering albedo,
  anisotropy (g factor). Per-volume local overrides via scene components.

### 3.6c — God rays / volumetric light shafts

Screen-space approximation using radial blur (epipolar sampling):

- Shadow map is used to mask occluded samples
- Radial blur from sun NDC position, 8 samples per pass × 3 passes
- Additive blend into colour buffer
- Attenuated by the aerial perspective transmittance at the screen position
- Works for sun, spot lights with high intensity; moon at night

### 3.6d — Volumetric clouds

Layered noise raymarching. Optional; expensive (1–4ms at 1440p with temporal reprojection).

- **Cloud shape**: base noise (Perlin-Worley FBM) + detail noise (Worley 3-octave);
  weather map (RGBA: coverage, precipitation, cloud type, height gradient)
- **Lighting**: single-scattering with powder term (Henyey-Greenstein dual lobe);
  ambient SH contribution from atmosphere; no multi-scatter (approximated by powder + ambient)
- **Temporal reprojection**: quarter-resolution cloud buffer re-projected each frame;
  full-res composite with edge-aware upsample. Convergence in ~8 frames.
- **Wind animation**: UVW offset over time using a wind direction + speed parameter
- **Weather system hooks**: precipitation density modulates fog injection (3.6b) and
  triggers wet-surface material parameter modulation

### 3.6e — Aurora borealis

Animated curtain emission volume above the cloud layer. Procedural via a screen-space
ray cast against a thin volume at 100–200km altitude. Phase function is forward-scattering;
colour follows a 1D LUT (green/red/blue spectrum). Temporal reprojection at 1/4 res.
Triggered by a scene-level `aurora_intensity` parameter (0 = disabled, no GPU cost).

### 3.6f — Weather system

Ties atmosphere, fog, and particles together:

- **Rain**: GPU particles emitted from a volume above the camera; streak motion blur;
  ripple normal map animated on wet surfaces; splash decals near the ground
- **Snow**: slower GPU particles, accumulation map modulates surface albedo over time
- **Dust / sandstorm**: high-frequency fog density, directional scattering tint
- **Wet surfaces**: `wetness` material parameter (0–1) modulates roughness → 0,
  darkens albedo slightly (Oren-Nayar film); driven by the weather system

---

## 3.7 — Post-processing stack

All post-processing runs as a chain of frame graph passes after the main colour pass
and before tone mapping / swapchain blit. Each pass is independently toggleable; the
frame graph barrier system handles the image transitions automatically.

### 3.7a — Bloom

- **Threshold extract** (compute): isolate pixels above luminance threshold;
  also samples emissive buffer directly (no threshold needed for emissive)
- **Dual-Kawase downsample chain** (6 levels): 13-tap filter at each level
- **Upsample chain** with additive blend (Jimenez 2014 physical bloom)
- **Lens dirt mask**: texture overlay multiplied into the bloom result before add-blend
- **Physically-based lens bloom**: anamorphic streaks (horizontal Gaussian at 1/4 height)
  for anamorphic lens simulation

### 3.7b — SSAO / GTAO

- **SSAO** (quick option): hemisphere-sampled screen-space rays in view space;
  depth-dependent radius; bilateral blur; ~0.4ms at 1440p
- **GTAO** (production option): ground-truth ambient occlusion (Jimenez 2021);
  bent normals output for directional occlusion; multi-bounce approximation via
  a SH projection; ~1.2ms at 1440p with temporal accumulation

### 3.7c — Screen-Space Reflections (SSR)

- HiZ-traversal ray marching in screen space (Stachowiak 2015)
- Cone-traced roughness blending: glossy at low roughness, fades to IBL at high roughness
- Temporal accumulation with motion vector reprojection
- Fallback to prefiltered IBL when the reflection leaves the screen
- ~0.8ms at 1440p quarter-resolution + upsample

### 3.7d — Temporal Anti-Aliasing (TAA)

- Halton(2,3) jitter sequence applied to the projection matrix each frame
- 16-tap nearest-neighbour history reprojection using a velocity buffer
- Variance clipping (Liang 2016) to prevent ghosting
- Sharpening pass (bicubic 5-tap) after TAA to recover lost detail
- Prerequisite for SSR, SSGI, DDGI temporal reprojection

### 3.7e — Color grading

- **Neutral ACES** tone map (default; physically calibrated)
- **LUT-based grading**: 64³ or 32³ RGB LUT baked from colour grading software;
  `.cube` file format supported by the asset cooker → cooked to 3D TextureResource
- **Chromatic aberration**: radial UV offset (lateral CA); intensity parameter
- **Vignette**: cos⁴ falloff, colour and intensity parameters
- **Film grain**: temporal blue-noise dithered grain; grain size + intensity parameters
- **Color temperature**: Kelvin → RGB matrix applied before the LUT

### 3.7f — Depth of Field

- **CoC compute pass**: circle-of-confusion radius from depth + camera aperture, focus
  distance, focal length; stored in a half-res buffer
- **Scatter-gather blur**: hexagonal bokeh kernel (3-pass: 2 diagonal + 1 horizontal);
  near/far CoC treated separately to avoid background-leaks-into-foreground artefact
- **Near field composite**: near-field blur composited over sharp image using alpha mask
- ~1.0ms at 1440p

### 3.7g — Motion Blur

- **Per-object velocity**: motion vectors from previous-frame transform (stored in velocity buffer)
- **Camera motion**: dominant camera rotation term as a screen-space velocity
- **Tile max filter**: 20×20 tile max velocity → neighbourhood max to avoid halo artefacts
- **Gather reconstruction filter**: 8-sample gather along tile velocity; depth-tested
- ~0.6ms at 1440p

### 3.7h — Lens effects

- **Lens flare**: screen-space occlusion query on the sun disc position; flare sprites
  (ghosts + streaks) composited along the sun-to-screen-centre axis
- **Anamorphic streaks**: horizontal Gaussian streak at 1/4 height (built into bloom 3.7a)
- **Screen-space dirt mask**: texture overlay multiplied onto lens flare + bloom (same mask as 3.7a)

### 3.7i — Screen-space SSS blur pass

Second ship of SSS (companion to 3.5f screen-space):

- Horizontal + vertical separable Gaussian in the SSS irradiance buffer
- 3 pass widths (RGB channels); stencil-masked to SSS pixels only
- Frame graph reads from main colour buffer, writes to SSS buffer; composite pass blends result

### 3.7j — Upscaling integration

- **AMD FidelityFX Super Resolution 3 (FSR 3)**: temporal upscaling + frame interpolation;
  targets render resolutions of 50–77% of display resolution
- **NVIDIA DLSS 3.x**: optional; requires DLSS SDK (proprietary, desktop-only)
- **Intel XeSS**: optional; ONNX model-based temporal upscaling
- Cerid provides a `IUpscaler` interface; FSR 3 is the default open implementation.
  Replaces TAA when active (TAA and upscaling are mutually exclusive).

---

## 3.8 — GPU-driven rendering + particles + water

Infrastructure for high-count geometry, simulation-driven visual effects, and
large-scale environments.

### 3.8a — Hi-Z occlusion culling

- Depth pyramid construction (compute, mip chain from depth buffer each frame)
- GPU occlusion query: per-object AABB tested against Hi-Z; results read back with
  one-frame latency (async compute overlap)
- Objects that fail are not submitted to indirect draw; zero draw-call overhead

### 3.8b — GPU-driven indirect rendering

- **Scene buffer**: all object transforms + material indices in a persistent `StorageBuffer`
- **Compute cull pass**: frustum + Hi-Z occlusion → writes `VkDrawIndexedIndirectCommand`
  structs into a draw-call buffer; `VkDrawIndirectCount` for variable counts
- **Depth pre-pass**: indirect draw with vertex-only pipeline (null fragment shader)
- **Colour pass**: indirect draw with the full pipeline, material index passed via push constant
- Replaces per-object CPU `vkCmdDrawIndexed`; scales to 100k+ objects

### 3.8c — GPU particle system

- **Emit pass** (compute): spawn new particles according to emitter descriptors
  (position, velocity spread, lifetime, colour gradient, size curve)
- **Update pass** (compute): integrate velocity + gravity + drag; kill dead particles
  using atomic compact; force fields (spherical attract/repel, directional wind)
- **Sort pass** (compute): radix sort particles by depth for correct translucent blending
- **Render**: `VkDrawIndirectCommand` from compact output; billboard quads or lit spheres
- **Ribbon / trail**: per-particle history buffer → procedural quad strip each frame

### 3.8d — GPU particle VFX library

Pre-authored emitter templates (serialised to CRDR assets, editable in the editor):

| Effect | Technique |
|--------|-----------|
| Fire | Billow noise-animated sprites, additive blend, no shadow receive |
| Smoke | Soft particles, back-lit by directional light, depth fade |
| Explosion | Shockwave decal + debris particles + smoke + flash billboard |
| Magic sparks | HDR emissive point sprites + motion blur streak |
| Dust motes | Small sphere particles, forward scatter, view-aligned spawn volume |
| Waterfall / splash | Collision against depth buffer (screen-space), foam spawn on impact |

### 3.8e — Ocean / water surface

- **Gerstner wave sum**: analytic displacement (6–12 wave trains); cheap, art-directable;
  tessellated quad mesh with LOD via distance-based subdivision
- **FFT wave simulation** (higher quality, separate mode): compute FFT of Phillips spectrum;
  inverse FFT to displacement + normal; tile-able; updates every frame at half-res
- **Foam map**: white-cap foam from wave crest jacobian (negative jacobian = foam); animated
  foam texture overlaid at crest regions
- **Water shading**: Fresnel-blended surface (sky reflection from 3.6a + SSR from 3.7c);
  deep-water colour via Beer-Lambert transmittance; subsurface scattering tint (3.5f);
  spray particle injection at wave crests (3.8c)
- **Underwater**: fog density + caustic projection when camera dips below surface

### 3.8f — Caustics

Screen-space caustic projector:

- Shadow map from the light's perspective, water surface normals refract ray directions,
  photon splat onto receiver surfaces (additive blend)
- Animated via the wave displacement map
- Separate from full caustic simulation (path-traced caustics are Phase 5)

### 3.8g — Dynamic decals

- `MaterialDomain::Decal` materials projected onto geometry within an OBB volume
- Written during a dedicated decal pass (reads G-buffer position, applies material blend)
- Applications: blood splatters, impact marks, wet footprints, graffiti
- Up to 256 active decals; GPU-culled per frame

---

## 3.9 — Global illumination

Pre-RT GI options. Multiple techniques implemented as progressive layers; the
highest-quality enabled technique wins per scene.

### 3.9a — Screen-Space GI (SSGI)

- Half-res hemisphere ray marching in screen space (4 rays per pixel × 8 steps)
- Samples colour buffer → accumulates indirect radiance
- Temporal accumulation with variance clipping (same infrastructure as TAA)
- Falls back to IBL outside the screen; cheap and convincing for interior scenes
- ~1.5ms at 1440p half-resolution

### 3.9b — Dynamic Diffuse GI (DDGI)

Probe-based irradiance caching (Majercik 2019, updated 2021):

- **Probe grid**: uniform 3D grid of spherical irradiance probes; configurable spacing
  (1–4m typical for room-scale; auto-placed in scene via AABB)
- **Ray tracing** (compute + BVH; requires the BVH from 3.8a): 64 rays per probe per
  frame; traces to scene geometry and accumulates radiance from direct lights + the sky
- **Irradiance / depth atlas**: probe data encoded into two 2D texture atlases
  (6×6 irradiance octahedral + 14×14 depth for backface filtering)
- **Apply pass**: every surface queries nearest 8 probes (trilinear interpolation);
  irradiance weighted by visibility distance
- **Temporal accumulation**: hysteresis blend (0.97 history weight); probes self-correct
  after light changes in ~33 frames
- **Relighting**: probe irradiance recomputes on scene light change (not instant;
  convergence in 1–2s for large scenes)
- ~2–4ms at 1440p depending on scene complexity and probe count

### 3.9c — Radiance Cascades

Alexander Sannikov's 2024 algorithm — a hierarchical radiance cache that avoids
the probe placement problems of DDGI:

- Multi-level cascade of radiance probes; each level has coarser spatial resolution
  but finer angular resolution
- Cascades merged from coarse to fine each frame; no separate ray-trace step
  (merged from previous-frame cascade data + current-frame screen-space samples)
- Works in 2D and 3D; particularly elegant for the 2D (top-down, side-scroller) case
- Experimental at this roadmap stage; implementation follows the reference paper + open
  source reference implementations. May replace DDGI if quality/perf profile is superior.

### 3.9d — Lightmap baking pipeline

Offline GI for static geometry (recommended for architectural visualisation, cinematic):

- CPU-side pathtracer (one-time bake, not real-time): progressive multi-bounce Monte Carlo
- Output: `TextureResource` lightmap atlas packed at cook time
- Scene lightmap UVs baked per mesh at import time (xatlas or Blender UV unwrap)
- Applied in the forward shader as a second UV set at `set 1, binding N`; replaces diffuse
  GI from DDGI for lightmapped surfaces
- The asset cooker runs the bake; result stored as a LMAP artifact

### 3.9e — Bent normals + directional AO

Derived from GTAO (3.7b):

- Bent normal = average unoccluded hemisphere direction; stored as a half-res RGB buffer
- Directional occlusion: weight the IBL sample direction by the bent normal cone
- Eliminates the light-bleeds-through-corners artefact common with non-directional SSAO
- Free when GTAO is already running; just adds the bent normal output target

---

## Decisions

- ADR-0016 — Forward+ render path (first `IRenderPath`)
- ADR-0017 — Deferred + Visibility Buffer paths (Phase 5)
- ADR-0018 — Physics architecture (PhysX backend, native later)
- ADR-0020 — Scene & ECS hybrid + UI in scene tree
- ADR-0021 — Animation architecture
- ADR-0022 — Open-world streaming pipeline
- ADR-0047 — Font rendering (MTSDF + HarfBuzz)
- *(Atmosphere ADR to be authored before 3.6a starts)*
- *(GI ADR to be authored before 3.9b/3.9c starts — DDGI vs Radiance Cascades decision)*

---

## Visual effects summary (quick reference)

| Category | Technique | Phase | Render cost (1440p) |
|----------|-----------|-------|---------------------|
| Shading model | Cook-Torrance GGX PBR | 3.5b | shader-time |
| Shading model | IBL (irradiance + prefilter + BRDF) | 3.5c | texture lookup |
| Lighting | CSM (4 cascades, PCF) | 3.5d | ~1ms shadow maps |
| Lighting | PCSS soft shadows | 3.5d | ~0.5ms |
| Lighting | Area lights (LTC) | 3.5h | shader-time |
| Shading model | Clear coat | 3.5e | shader-time |
| Shading model | Anisotropic specular | 3.5e | shader-time |
| Shading model | Cloth / velvet | 3.5e | shader-time |
| Shading model | Iridescence | 3.5e | shader-time |
| Shading model | Transmission / refraction | 3.5e | texture lookup |
| Shading model | Subsurface scattering (pre-integrated) | 3.5f | LUT lookup |
| Shading model | Subsurface scattering (screen-space) | 3.5f | ~0.3ms |
| Shading model | Toon / cel shading | 3.5i | shader-time |
| Shading model | Outline pass (inverted hull) | 3.5i | extra draw pass |
| Atmosphere | Sky atmosphere (Hillaire LUTs) | 3.6a | ~0.5ms |
| Atmosphere | Volumetric fog (froxel) | 3.6b | ~1ms |
| Atmosphere | God rays | 3.6c | ~0.2ms |
| Atmosphere | Volumetric clouds | 3.6d | 1–4ms |
| Atmosphere | Aurora borealis | 3.6e | ~0.3ms |
| Post-process | Bloom (dual-kawase) | 3.7a | ~0.4ms |
| Post-process | SSAO | 3.7b | ~0.4ms |
| Post-process | GTAO + bent normals | 3.7b | ~1.2ms |
| Post-process | SSR | 3.7c | ~0.8ms |
| Post-process | TAA | 3.7d | ~0.3ms |
| Post-process | Color grading LUT | 3.7e | ~0.1ms |
| Post-process | Depth of field | 3.7f | ~1.0ms |
| Post-process | Motion blur | 3.7g | ~0.6ms |
| Post-process | Lens flare | 3.7h | ~0.1ms |
| Post-process | Upscaling (FSR 3 / DLSS) | 3.7j | ~0.5ms |
| Simulation | GPU particles | 3.8c | variable |
| Simulation | Ocean / water surface | 3.8e | ~1ms (Gerstner) |
| Simulation | Dynamic decals | 3.8g | ~0.2ms |
| GI | SSGI | 3.9a | ~1.5ms |
| GI | DDGI probe-based | 3.9b | 2–4ms |
| GI | Radiance cascades | 3.9c | TBD |
| GI | Lightmap baking | 3.9d | offline bake |
| RT (Phase 5) | RT AO + reflections + GI | 5.x | 2–6ms with DLSS |
