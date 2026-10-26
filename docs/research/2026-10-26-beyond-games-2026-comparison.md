# Cerid beyond games: the 2026 state of the art, area by area

<!-- doc-role: reference -->
> Technical reference and proposals; verify dated claims against current contracts/source. Live slice state exists only in
> [ROADMAP](../ROADMAP.md). Rules: [AGENTS](../../AGENTS.md). Agent entry: [START_HERE](../../START_HERE.md).

**Date:** 2026-10-26. **Status:** Recorded research, part 2 of 3. Every proposed row below is a proposal for the user,
not an accepted change. Nothing in [ROADMAP](../ROADMAP.md) was edited by this research.

## Question, method and evidence boundary

What do the best tools, engines, libraries and standards ship in 2026 for the domains beyond games (audio and DAWs,
video, media and cinematics, engineering simulation, CAD, scientific computing, web applications, networking and
collaboration, robotics, aerospace and embedded work), how does Cerid compare, and which missing pieces should become
roadmap rows? The bar is the user's standing order: the proper, full-feature, gold-standard implementation, never the
"simple" or "modern only" option.

- **Part 1 is not repeated.** [Part 1](2026-10-26-state-of-the-art-2026-comparison.md) covers GPU APIs, rendering,
  engine architecture, diagnostics, physics, ML, assets, UI, platforms, agent tooling, computational geometry and
  navigation. Where an area overlaps (ACES 2.0 and OCIO 2.5, OpenUSD Core Spec 1.0, DirectStorage, WebGPU browser status,
  Box2D determinism, the MCP security pin, Jolt/PhysX/Newton peers, HarfBuzz, geodesy and road routing), this document
  links Part 1's row instead of researching it again, and reuses its proposed IDs ([GPU-TIER.0, RAH-5.d, JOBS-QOS,
  GEOM-K.4, GEOM-EXACT.1, NAV.3, NAV.6](2026-10-26-state-of-the-art-2026-comparison.md#gaps-and-proposed-roadmap-changes))
  as dependencies.
- **Cerid side.** Read, not run: [ROADMAP](../ROADMAP.md#master-table) rows from Order 501 to 908, the
  [rendering/UI contracts](../design/rendering-ui-contracts.md) for MED and PLG, the
  [whole-system review](2026-09-12-cerid-whole-system-review.md) findings G11 to G25,
  [ADR-0065](../decisions/0065-hesap-numerical-substrate.md), [ADR-0077](../decisions/0077-multi-domain-expansion-vision.md),
  [ADR-0081](../decisions/0081-agent-native-engine-cli.md), [ADR-0087](../decisions/0087-large-scale-deterministic-simulation.md),
  [ADR-0108](../decisions/0108-ceir-owned-language-stack-supersedes-cpp-only-scripting.md),
  [ADR-0129](../decisions/0129-renderer-ui-editor-delivery-order.md), [ADR-0130](../decisions/0130-system-qualification-and-agent-driven-products.md),
  [ADR-0063](../decisions/0063-eylem-determinism-contract.md), the
  [user decisions session](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md) and source listings and
  greps of `engine/` on 2026-10-26. A grep hit is a source fact, not a runtime or quality claim. No build, test, benchmark
  or CI ran for this document.
- **Outside side.** Web pages read on 2026-10-26 (search results or fetched pages). Sources are listed at the end as
  B-numbers (Part 1 uses W-numbers) with their publication dates. **(K)** marks a link given from prior knowledge and
  not re-read today; its current version or status is uncertain. "Uncertain" means the sources disagree or were not
  conclusive.
- **No downloads** were made. Items that can only be judged with a download are in [Needs a download](#needs-a-download).

## The principles every area is judged by

These come from the user's standing order and the 2026-10-26 decisions; each area section ends with how they apply.

- **(a) Runtime changeability.** Audio graphs, simulation models, media pipelines, cinematics, notebooks and network
  rules are authored in CEIR/CHIR/CKIR, edited live and swapped without restarting or rebuilding the engine, with the
  staged commit, last-good generation and completion-driven retirement of [RAH-7](../ROADMAP.md#slice-rah-7).
- **(b) crd-hesap is the computational base.** Every solver, matrix, decomposition, optimizer, ODE/PDE, FFT, statistics
  and interpolation need uses crd-hesap; crd-math holds only fixed-size small types ([OWN.1](../ROADMAP.md#slice-own.1),
  [OWN.2](../ROADMAP.md#slice-own.2)).
- **(c) CHIR is the Cerid language** on top of the engine (MATLAB-class through [CHIR-SCI](../ROADMAP.md#slice-chir-sci));
  commands, scripts, cheats, mods and agent actions use it and one shared command/reflection model.
- **(d) Strict ownership inside the engine.** Every capability lives in one owning module; mods, plugins, user projects
  and scripts may implement anything.
- **(e) Wide hardware.** SSE2 x64 floor with runtime dispatch ([SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch)),
  older-GPU fallbacks, Windows/Linux first, macOS and web next.
- **(f) Budgets from peers.** Beat the best peer; parity only when no lever remains, with that evidence recorded.

## Highest-impact gaps first

Ranked by how much later work each one blocks, how far Cerid is from the 2026 reference, and principles (a) to (f).
Details and placements are in [Gaps and proposed roadmap changes](#gaps-and-proposed-roadmap-changes).

| Rank | Gap | Cerid today | Proposed owner |
|---:|---|---|---|
| 1 | Domain rows duplicate capabilities or put them in the wrong module (autodiff in crd-math, two ROS 2 bridges, two URDF importers, two aero/atmosphere models, two "Path planning" rows, an internal PCG in eylem, plotting in three places) | Present in ROADMAP rows 8.2b, 8.0c/4.2e, 8.0a/EYLEM-v6c-urdf, 8.1a/EYLEM-v6f-aero, CONTROL.10/13, EYLEM-v7a, CHIR-SCI.3/NOTEBOOK.2/SCIVIZ.2 | Amend OWN.1; amend the named rows |
| 2 | Plotting is owned by SCIVIZ (Later, Order 772) but the notebook and CHIR-SCI need it at Order 518-522 | Order conflict; no plotting module in source | New SCIVIZ-PLOT before NOTEBOOK.2 |
| 3 | No distributed-memory numerics: no MPI-class layer, distributed sparse/AMG/direct, domain decomposition or multi-GPU solves | Missing (`hesap-comms` is modulation/OFDM, not message passing); 6.4 is a Later placeholder | New HESAP-DIST |
| 4 | No discretization core or meshing for engineering simulation: no FE function spaces/assembly, FV core, tet/hex/boundary-layer meshing | Missing; FEA.1-6 and CFD.1-4 are one-line placeholders with no Requires | New MESH-SIM.1-3, FE-CORE, FV-CORE; amend FEA/CFD |
| 5 | Audio device layer: render only, WASAPI shared mode only, no capture, no exclusive/low-latency path, no ASIO/CoreAudio/JACK | Partial (`audio_device_wasapi.cpp`: `eRender`, `AUDCLNT_SHAREMODE_SHARED`); PLG-1 covers Linux render | New AUDIO-DRV |
| 6 | Live authored audio graph replacement and game-audio middleware behaviour (events, RTPC, voice virtualization, interactive music) | Offline graph and a separate fixed voice mixer; AUDIO-CEIR.1 names state migration | Amend AUDIO-CEIR.1; new AUDIO-GAME |
| 7 | Spatial audio: Ambisonics, SOFA HRTF, object/bed rendering, ADM/IAMF, geometric acoustics | One clause in AUDIO-CEIR.4 ("where declared") | New AUDIO-SPATIAL |
| 8 | Hardware video decode/encode (Vulkan Video, D3D12 Video) and an explicit codec licensing decision | Missing in source; MED-GPU-1 names "Vulkan Video capability"; HEVC/AAC excluded by name; MED-REVIEW open | New MED-HW; amend MED-REVIEW |
| 9 | Plugin hosting completeness: per-plugin out-of-process hosting, plugin editor windows in crd-ui, AU/AUv3, LV2, ARA 2 | PLG-3 is "GUI-less headless first"; PLG-6 isolates only scanning; no AU/LV2/ARA rows | New PLG-7, PLG-8 |
| 10 | MIDI 2.0 transport: UMP endpoints through Windows MIDI Services, ALSA UMP and CoreMIDI; MIDI-CI | MIDI 2.0 data model and SMF parse only (`midi.hpp`) | New MIDI-UMP |
| 11 | Audio DSP algorithms the DAW needs: time-stretch and pitch-shift, loudness and true peak, dither, low-latency partitioned convolution | Missing (no hits in `hesap-dsp`); `multirate.hpp` exists | New HESAP-AUDIO-DSP (owner hesap-dsp) |
| 12 | Symbolic algebra (CAS): needed by MATLAB parity, manufactured solutions, acausal modeling and CAD constraints | Missing; graph AD exists in `hesap-autodiff` | New HESAP-SYM |
| 13 | Verification, validation and uncertainty quantification as shared machinery | Missing as a row; building blocks exist (`smolyak.hpp`, `mcmc.hpp`) | New VV-UQ |
| 14 | Web application runtime: Wasm 3.0 baseline, runtime CEIR to Wasm code generation (the web's live swap), isolation headers, WebGPU compatibility tier, OPFS, Wasm components as the isolated plugin format | WEB.1-5 name services, not these mechanisms | Amend WEB.1-5, EXT.3; new WEB-CEIR |
| 15 | CAD modeling core: geometric constraint solver, persistent topological naming, STEP AP242 edition 4 | CAD.1-3 placeholders; BREP.2 names stable feature naming | New CAD-SOLVE; amend CAD.1, BREP.3 |

Next in impact, with rows below: acausal (Modelica-class) modeling and FMU export (SIM.4, SIM.2); the full netcode
spectrum, dedicated servers and anti-cheat (4.2c, NET-SERVER, ANTICHEAT); CRDT choice (COLLAB.1); virtual production
(VPROD); the sequencer, camera/lens and render-farm rows (8.3a to 8.3c); EXR 3.4 parity (MED-2); ROS 2 and sensor peers
(4.2e, SENSOR.2); MAVLink autopilot bridge (AUTOPILOT); SPICE and orbits (3.1.19); MISRA/DO-178C evidence (EMBED.2).

Structural note, not ranked: almost every row in this document's scope is Later and sits after the renderer, UI,
editor and notebook ([ADR-0129](../decisions/0129-renderer-ui-editor-delivery-order.md)). That order is the user's
choice. It means "Cerid does DAWs, engineering simulation and CAD" is today a set of retained contracts plus a strong
numerical base, not shipped capability; the tables below say so cell by cell.

## Cerid's strengths these domains build on

Stated once so the gap tables are read fairly.

- **crd-hesap** has 25 modules in source (the core plus 24 `hesap-*` modules): dense (BLAS 1-3, LU/QR/Cholesky, SVD, MRRR, dqds), sparse (formats, Matrix
  Market, SELL/BSR), direct (multifrontal LU/LDLT/Cholesky, BLR, HSS, mixed-precision refinement), iterative (CG, GMRES,
  BiCGSTAB, IDR(s), MINRES, LSQR/LSMR, block variants), preconditioners (ILU family, SPAI, Schwarz, Chebyshev), AMG
  (Ruge-Stuben and aggregation), ordering (AMD, nested dissection, RCM, MC64), eigen (Lanczos, Krylov-Schur, LOBPCG,
  FEAST, Jacobi-Davidson), ODE (ERK, BDF, IMEX, DAE with structural analysis, events, dense output), optimization
  (L-BFGS-B, Levenberg-Marquardt, COBYLA, BOBYQA, CMA-ES, conic, KKT), quadrature (Gauss-Kronrod, QAGS, Lebedev,
  Smolyak, cubature), interpolation (splines, Akima, RBF, kriging, Clough-Tocher), FFT (real, N-D, NUFFT, sparse, DCT),
  DSP (FIR/IIR design, Remez, multirate, spectral, multitaper, state space), wavelets, special functions, statistics
  (distributions, MCMC with diagnostics, hypothesis tests, KDE), tensors (einsum, decompositions, DLPack), forward and
  reverse autodiff (graph AD, Hessian-vector, implicit differentiation) and motion profiles (Ruckig-class OTG, NURBS,
  clothoids). Its deterministic, allocation-free contract already cites DO-178C and MISRA C++:2023
  ([ADR-0095](../decisions/0095-hesap-numerical-analysis-motion-cluster.md)).
- **Audio** (`engine/media/audio`, about 4,700 lines): an offline graph renderer with per-sample automation in exact
  rational time and byte-identical output (the gate), a lock-free SPSC command ring and preallocated voice mixer for the
  real-time layer, a WASAPI render device with floating-point-mode and sample-domain checks (DIAG.10c), WAV/AIFF/FLAC
  and a MIDI 2.0-native data model (32-bit velocity, per-note controllers) with an SMF parser.
- **Timeline** (`engine/world/timeline`): an OTIO-shaped timeline resource that references media rather than embedding it.
- **Contracts already written** for the plugin host, the transcode engine, collaboration with an authoritative project
  host, secure networking, co-simulation, sensors, embedded generation, EDA and CAM (whole-system review G11 to G25).

<a id="area-1-audio"></a>
## 1. Audio and DAW

### State of the art 2026

- **Plugin formats.** VST 3.8 moved the VST3 SDK to the MIT license on 2025-10-29; ASIO moved to GPLv3 with a
  proprietary option at the same time [B01](#b01). CLAP is at 1.2.x (1.2.6 on 2025-03-11; the changelog now reaches
  1.2.10 with draft parameter-hover, parameter-origin, flush-events, background-activation and webview extensions, dates
  uncertain) [B02](#b02). ARA 2 (Melodyne-class audio access) has been Apache 2.0 since 2021 [B03](#b03). AUv3 on Apple and
  LV2 on Linux remain the native formats there (K).
- **MIDI 2.0.** Windows MIDI Services reached general availability on 2026-02-17 for Windows 11 24H2/25H2, with an in-box
  MIDI 2.0 kernel driver and a separately installed SDK runtime [B04](#b04). Linux carries UMP in the kernel since 6.5
  through rawmidi and the ALSA sequencer, with MIDI-CI in user space [B05](#b05). CoreMIDI has had UMP since macOS 11 (K).
- **Drivers.** WASAPI exclusive mode and `IAudioClient3` small shared-mode periods (K); ASIO now GPLv3 [B01](#b01);
  CoreAudio; PipeWire 1.6.x with JACK API compatibility fixes (release date uncertain) [B12](#b12); JACK2 (K).
- **Spatial.** Steam Audio (HRTF, Ambisonics, geometric and baked acoustics, custom SOFA HRTFs) has been fully Apache 2.0
  since 2024-02-20 [B06](#b06). IAMF (AOMedia, royalty-free; "Eclipsa Audio") is accepted by YouTube since January 2025,
  with an open DAW plugin and reference renderer [B07](#b07). ITU-R BS.2076 ADM and BS.2127 renderer, MPEG-H 3D Audio,
  Dolby Atmos (bed plus objects), AES69 SOFA and ITU-R BS.1770 loudness (K).
- **DSP.** Rubber Band 3 added the R3 "finer" engine (2022), 4.0 (2024-10-25) a live pitch shifter [B08](#b08);
  Signalsmith Stretch is MIT [B09](#b09); zplane élastique is the commercial reference (K).
- **Game audio.** Wwise 2025.1 (media pool, similar-sound search, side-chaining, playback limits) [B10](#b10); FMOD Studio
  2.03 and Unreal MetaSounds (sample-accurate, graph-authored procedural audio) (K).
- **DAWs.** Bitwig Studio 6 (March 2026; automation clips, clip aliases) [B11](#b11); Ableton Live 12, Reaper 7, Studio One,
  Logic Pro (K). Common core: plugin delay compensation across sends and side-chains, comping, warp/stretch markers,
  freeze and render-in-place, clip launching, modulation routing, MPE, surround and Atmos export.

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Real-time callback contract | CLAP thread contract; JUCE/Reaper practice (K) | Has: no alloc/lock on the callback, SPSC ring, preallocated voices (`audio_realtime.hpp`) |
| Sample-accurate automation | CLAP sample-offset events [B02](#b02); VST3 parameter queues (K) | Partial: offline graph per-sample in rational time; real-time commands carry `at_frame` but apply at block start (G14) |
| Offline bounce determinism | Most DAWs do not promise byte-identical bounces (K) | Ahead: byte-identical offline render is the gate; [AUDIO-CEIR.3](../ROADMAP.md#slice-audio-ceir.3) adds real-time/offline equivalence |
| Lock-free authored graph with feedback/delay, PDC | Bitwig, Reaper, MetaSounds (K) | Planned: [AUDIO-CEIR.1](../ROADMAP.md#slice-audio-ceir.1), [AUDIO-CEIR.2](../ROADMAP.md#slice-audio-ceir.2) |
| Device layer | WASAPI exclusive and `IAudioClient3`, ASIO, CoreAudio, PipeWire/JACK | Partial: WASAPI shared render only; Linux render planned in [PLG-1](../ROADMAP.md#slice-plg-1); capture, exclusive, ASIO, JACK missing |
| CLAP / VST3 / VST2 hosting | CLAP 1.2.x; VST 3.8 MIT [B01](#b01) [B02](#b02) | Planned: [PLG-3](../ROADMAP.md#slice-plg-3), [PLG-4](../ROADMAP.md#slice-plg-4), [PLG-5](../ROADMAP.md#slice-plg-5) |
| AU/AUv3, LV2, ARA 2 | Native on macOS/Linux; ARA Apache 2.0 [B03](#b03) | Missing (ADR-0077 section 3.4 lists AU and LV2; no row) |
| Plugin editor windows | CLAP `gui`, VST3 `IPlugView`, embedding in host windows (K) | Missing: PLG-3 is "GUI-less headless first"; no owner for embedding in crd-ui |
| Crash-isolated hosting | Bitwig per-plugin sandboxing, Reaper bridging (K) | Partial: PLG-6 isolates scanning only |
| MIDI 2.0 | UMP endpoints, MIDI-CI, Property Exchange on Windows/Linux/macOS [B04](#b04) [B05](#b05) | Partial: MIDI 2.0 data model and SMF parse; no UMP transport or MIDI-CI |
| Spatial audio | Steam Audio, IAMF, ADM, Atmos-class renderers [B06](#b06) [B07](#b07) | Planned in one clause of [AUDIO-CEIR.4](../ROADMAP.md#slice-audio-ceir.4); missing in source |
| Time-stretch, pitch, formants | Rubber Band R3, élastique, Signalsmith [B08](#b08) [B09](#b09) | Missing |
| Loudness, true peak, dither | BS.1770, EBU R128 meters in every DAW (K) | Missing |
| Game-audio behaviour | Wwise 2025.1, FMOD, MetaSounds [B10](#b10) | Missing as a row (voice mixer only) |
| DAW application | Bitwig 6, Ableton 12, Reaper 7, Logic, Studio One [B11](#b11) | Later: [DAW-APP.1](../ROADMAP.md#slice-daw-app.1) to [DAW-APP.3](../ROADMAP.md#slice-daw-app.3) |

### Gaps in area 1

- **The device layer is the floor of every audio product.** Capture (recording) does not exist. Exclusive mode and
  `IAudioClient3` low-latency periods, ASIO (now GPLv3, which needs a licensing decision for a non-GPL Cerid), CoreAudio
  (with MAC), native PipeWire plus the JACK API, aggregate and multi-device clocking, device loss and hot-plug, and
  drift-corrected duplex streams all need an owner below AUDIO-CEIR.
- **Plugins are scoped as headless audio processors.** A DAW user opens plugin editors; that needs window embedding
  into crd-ui on Win32, X11, Wayland and Cocoa, scale-factor handling and per-plugin out-of-process hosting (a crashing
  plugin must not stop the session, as in Bitwig). AU/AUv3, LV2 and ARA 2 have no rows.
- **MIDI 2.0 stops at the file.** The data model is right; the transport (UMP endpoints, groups, function blocks,
  jitter-reduction timestamps, MIDI-CI discovery and Property Exchange, MPE) is missing.
- **DSP for production audio** (time-stretch with transient and formant handling, high-quality pitch shift, loudness
  and true-peak metering, dither and noise shaping, low-latency non-uniform partitioned convolution, oversampling) is
  missing. Under (b) it belongs in `hesap-dsp`; audio and the DAW consume it.
- **Spatial audio** needs its own row: higher-order Ambisonics (AmbiX), SOFA HRTF with interpolation and near-field,
  object and bed rendering (VBAP, ADM BS.2076 import/export, IAMF), binaural monitoring, and geometric acoustics over
  `crd-geometry-bvh` (the "ray-traced audio" differentiator in ADR-0077).
- **Game audio** (events, states, switches, RTPC, voice limits and virtualization, priority, streaming banks,
  ducking, interactive music with beat-synchronous transitions, dialogue) is not named anywhere.
- **Principles.** (a) An audio graph edit must swap live without a click: the new generation is built off the audio
  thread, state (delay lines, filter memories, voices) migrates by node identity, and a short authored crossfade covers
  structural edits; this is AUDIO-CEIR.1's "state migration" made explicit. (b) Every DSP algorithm lives in
  `hesap-dsp`. (c) MetaSounds-style authoring is CHIR/CEIR graphs, not a second graph language. (d) The audio module
  owns devices and scheduling only. (e) The SSE2 floor applies to every DSP kernel through SIMD-DISPATCH; audio threads
  need the JOBS-QOS real-time class from Part 1. (f) Peers: Reaper or Bitwig for round-trip latency and DSP load at 32
  and 64-frame buffers, Rubber Band R3 and élastique for stretch quality (listening tests plus objective metrics),
  Steam Audio for spatial cost, Wwise/FMOD for voice counts.

<a id="area-2-media"></a>
## 2. Video, media and cinematics

### State of the art 2026

- **Hardware video.** Vulkan Video has decode and encode for H.264, H.265 and AV1 (AV1 encode with quantization maps
  from Vulkan 1.3.302, November 2024) [B13](#b13), VP9 decode from June 2025, and an intra-refresh encode extension
  (July 2025) [B14](#b14). D3D12 Video decode/encode/motion estimation and Media Foundation on Windows, VA-API on Linux,
  NVENC/NVDEC, AMD AMF and Intel oneVPL as vendor SDKs (K).
- **Codecs.** AV2 v1.0 was released by AOMedia (press release 2026-06-09; claims up to 40% bandwidth reduction over AV1,
  vendor figure) [B15](#b15). VVC/H.266 is pooled by Access Advance (over 5,400 patents; it absorbed the Via LA HEVC/VVC
  pool in December 2025), and Access Advance's separate Video Distribution pool asserts patents on streaming that
  includes AV1 and VP9 [B16](#b16). AV1 remains royalty-free under the AOMedia license (K); the distribution-pool claim is a
  risk to record, not a settled fact.
- **Image formats for film.** OpenEXR 3.4 added lossless HTJ2K compression; the 3.4.x HTJ2K decoder had several
  memory-safety CVEs fixed through 3.4.14 [B17](#b17). Deep scanline/tiled EXR and multipart are the compositing
  standard (K).
- **Editorial and pipeline.** OpenTimelineIO 0.18.0 (2025-11-06) [B18](#b18); OpenUSD with Hydra 2.0 scene indices (K) and
  AOUSD Core Specification 1.0 ([Part 1 W58](2026-10-26-state-of-the-art-2026-comparison.md#w58)); ACES 2.0 and OCIO 2.5
  ([Part 1 area 2](2026-10-26-state-of-the-art-2026-comparison.md#2-rendering-techniques)).
- **Render farms.** Open Job Description (AWS, January 2024) describes portable render jobs and drives Deadline Cloud
  [B19](#b19); OpenCue (ASWF) and Deadline 10 (K).
- **Virtual production.** OpenTrackIO 1.0.x (SMPTE RIS) standardizes camera tracking and lens data with sub-frame
  timing, implemented in Unreal 5.6 per its author [B20](#b20); Unreal nDisplay LED-volume rendering with inner and outer
  frustums, genlock and SMPTE ST 2110 (K).
- **Editing and compositing.** DaVinci Resolve, Nuke (deep compositing, node graphs), Unreal Sequencer and Movie Render
  Graph (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Owned software codecs | FFmpeg/libavcodec, dav1d, SVT-AV1 (K) | Partial/planned: [MED-1](../ROADMAP.md#slice-med-1) Partial; [MED-2](../ROADMAP.md#slice-med-2) to [MED-12](../ROADMAP.md#slice-med-12) Open; HEVC and AAC excluded by name |
| Hardware decode/encode | Vulkan Video, D3D12 Video, NVENC/AMF/oneVPL [B13](#b13) [B14](#b14) | Missing in source; [MED-GPU-1](../ROADMAP.md#slice-med-gpu-1) names "Vulkan Video capability" |
| AV2, VVC | AV2 v1.0 [B15](#b15); VVC pooled [B16](#b16) | Missing; licensing posture unresolved ([MED-REVIEW](../ROADMAP.md#slice-med-review) notes an H.265 wording conflict) |
| EXR 3.4 parity | HTJ2K, deep, multipart [B17](#b17) | Partial: own EXR read/write; HTJ2K, deep and multipart not named |
| Timeline and editorial | OTIO 0.18 [B18](#b18); Sequencer (K) | Has: OTIO-shaped timeline resource (`timeline_resource.hpp`); [MED-GPU-3](../ROADMAP.md#slice-med-gpu-3), [8.3a](../ROADMAP.md#slice-8.3a), [DAW-APP.3](../ROADMAP.md#slice-daw-app.3) planned |
| Compositing and grading | Nuke, Resolve (K) | Planned: [MED-GPU-2](../ROADMAP.md#slice-med-gpu-2) |
| Camera and lens model | Physical cameras, lens files, distortion maps (K); OpenTrackIO [B20](#b20) | Planned in [8.3b](../ROADMAP.md#slice-8.3b) (presets, lens metadata, DOF) |
| Render farm | OpenJD, Deadline Cloud, OpenCue [B19](#b19) | Planned: [8.3c](../ROADMAP.md#slice-8.3c), [OPS.3](../ROADMAP.md#slice-ops.3) |
| Virtual production | nDisplay, genlock, OpenTrackIO [B20](#b20) | Missing |
| Hydra interop | Hydra 2.0 render delegates and scene indices (K) | Missing; [INTEROP.1](../ROADMAP.md#slice-interop.1) covers USD data only |

### Gaps in area 2

- **Two licensing postures, not one.** Owning an HEVC or VVC codec means owning its patent exposure; calling the OS or
  driver (Media Foundation, D3D12 Video, Vulkan Video, VA-API) uses the platform holder's licensed implementation.
  MED-REVIEW should decide both explicitly: owned royalty-free codecs (AV1, VP9, Opus, FLAC, AV2 once tools are stable)
  plus hardware/OS paths for H.264, HEVC and, where offered, VVC, with the AV1/VP9 distribution-pool claim recorded as a
  risk for legal review. This is not legal advice; it is the decision the row must record.
- **Hardware video has no row.** Decode into frame-graph images without a copy, encode from render targets, capability
  tiers per codec/profile/level on each adapter, and the owned software codec as the fallback (principle e).
- **EXR parity**: HTJ2K, deep data, multipart, DWAA/DWAB, mip/rip maps, and a fuzz corpus that covers the 3.4.x CVE
  classes, with OpenEXR as the peer.
- **Cinematics are three one-line rows.** A Sequencer-class sequencer binds any reflected property or CHIR value,
  supports spawnables, subsequences, shots, takes and camera cuts, and plays in the running game. The camera row needs a
  physical lens model shared with the offline renderer (G18). The farm row needs OpenJD job templates and adapters.
- **Virtual production and Hydra are missing.**
- **Principles.** (a) A media pipeline (decode, grade, composite, encode) is an authored graph; changing a node while
  a timeline plays swaps the generation at a frame boundary and re-renders only the cached ranges it affects; a
  sequencer edit applies to the running game. (b) Colour science, resampling, optical flow and DCT/wavelet transforms
  call hesap. (e) Hardware codec tiers fall back to software; older GPUs without Vulkan Video still decode. (f) Peers:
  FFmpeg with dav1d and SVT-AV1 for speed and quality (PSNR/SSIM/VMAF), OpenEXR for EXR throughput, NVENC/AMF for
  encode latency.

<a id="area-3-simulation"></a>
## 3. Engineering simulation

### State of the art 2026

- **Frameworks.** MFEM 4.8 (2025-09-18) and 4.9 (2025-12-11) with GPU partial assembly [B23](#b23); deal.II 9.x,
  FEniCSx/DOLFINx with the UFL form language, MOOSE (multiphysics on libMesh/PETSc), OpenFOAM (Foundation 12/13 and ESI
  v25xx), SU2 8.x with adjoints (K). Commercial reference capability: Ansys Mechanical/Fluent, Abaqus (contact,
  nonlinear materials, explicit dynamics), COMSOL (equation-based multiphysics) (K).
- **Solvers.** PETSc 3.24 (adds `PetscRegressor`) and 3.25 [B21](#b21); NVIDIA cuDSS with single-node multi-GPU (0.7)
  and multi-GPU multi-node through a user-supplied communication layer [B22](#b22); Trilinos, hypre (BoomerAMG), MUMPS,
  SuiteSparse, AmgX (K). MPI 4.1 and the MPI 5.0 standard (K, version status uncertain).
- **Meshing.** Gmsh 4.14.0 (2025-07-03, OCCT 7.8 binaries), 4.14.1 and 4.15.0 (changelog date 2025-10-26) [B27](#b27);
  Netgen/NGSolve, TetGen, fTetWild (robust tetrahedralization), hex-dominant and high-order curved meshing research (K).
- **Verification and validation.** ASME V&V 10-2019 (reaffirmed 2025) for solid mechanics [B26](#b26); ASME V&V 20 for
  CFD and heat transfer (K); the method of manufactured solutions is the code-verification standard, still rare for
  commercial nonlinear solid mechanics [B28](#b28). UQ: Dakota, UQLab, polynomial chaos and multilevel Monte Carlo (K).
- **Co-simulation.** FMI 3.0.2 (2024-11-27), FMI-LS-BUS 1.0 (2025-07-21) and 2026 betas of further layered standards
  [B24](#b24); SSP 2.0 (December 2024) adds FMI 3.0 and Modelica components [B25](#b25); OpenModelica and Dymola (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Sparse direct, iterative, AMG, eigen | PETSc, Trilinos, MUMPS, SuiteSparse, hypre [B21](#b21) | Has (shared memory): `hesap-direct`, `-iterative`, `-amg`, `-eigen`, `-preconditioners` with peer boards in [bench](../bench/README.md) |
| GPU sparse direct and iterative | cuDSS MG/MGMN [B22](#b22); AmgX (K) | Planned: [HGP-1](../ROADMAP.md#slice-hgp-1) |
| Distributed memory (MPI-class) | PETSc, Trilinos, hypre, MUMPS (K) | Missing; [6.4](../ROADMAP.md#slice-6.4) placeholder |
| FE discretization core | MFEM, deal.II, FEniCSx [B23](#b23) | Missing; [FEA.5](../ROADMAP.md#slice-fea.5) "Element library" placeholder |
| FV discretization core | OpenFOAM, SU2 (K) | Missing; [CFD.1](../ROADMAP.md#slice-cfd.1), [CFD.2](../ROADMAP.md#slice-cfd.2) placeholders |
| Meshing | Gmsh, Netgen, TetGen, fTetWild [B27](#b27) | Missing (Delaunay/CDT in geometry; no volume meshing for analysis) |
| Structural FEA | Abaqus, Ansys (K) | Later placeholders: [FEA.1](../ROADMAP.md#slice-fea.1) to [FEA.6](../ROADMAP.md#slice-fea.6) |
| CFD | OpenFOAM, SU2, Fluent (K) | Later placeholders: [CFD.1](../ROADMAP.md#slice-cfd.1) to [CFD.4](../ROADMAP.md#slice-cfd.4) |
| Real-time FEM | XPBD/FEM in games and medical sims (K) | Later: [EYLEM-v7a](../ROADMAP.md#slice-eylem-v7a) (still plans an internal PCG) |
| V&V and UQ | ASME V&V 10/20, MMS, Dakota [B26](#b26) [B28](#b28) | Missing as a row; hesap has Smolyak quadrature and MCMC |
| Co-simulation | FMI 3.0.2, SSP 2.0 [B24](#b24) [B25](#b25) | Later: [SIM.2](../ROADMAP.md#slice-sim.2) (FMI adapters) |
| Acausal modeling | Modelica, OpenModelica (K) | Missing; `hesap-ode` has DAE structural analysis |
| Digital twins | SIM/SENSOR substrates | Later: [SIM.1](../ROADMAP.md#slice-sim.1) to [SIM.3](../ROADMAP.md#slice-sim.3) |

### Gaps in area 3

- **Distributed numerics.** HPC is not a deployment detail: distributed vectors and sparse matrices, ghost exchange,
  parallel AMG, domain decomposition (BDDC, FETI-DP, overlapping Schwarz), a distributed sparse direct solver, graph
  partitioning, and multi-GPU solves on one owned communication interface (an MPI adapter plus a single-node shared
  memory and multi-GPU transport). Under (b) it is hesap's; 6.4 consumes it.
- **Discretization cores.** FE (H1, H(curl), H(div), L2 spaces to high order, quadrature from `hesap-quadrature`,
  sparse and matrix-free partial assembly on CPU and GPU, Dirichlet/Neumann/Robin and periodic conditions,
  hp-adaptivity) and FV (unstructured polyhedral cells, face fluxes, gradient reconstruction, limiters, SIMPLE/PISO and
  coupled pressure-velocity, turbulence models) are what every peer is built on. FEA, CFD, real-time FEM and the
  multiphysics row should share them.
- **Meshing for analysis** (CAD-surface and discrete-surface meshing, tetrahedral and hex/hex-dominant meshes,
  boundary-layer prisms, size fields, quality metrics, curved high-order elements, adaptive refinement) has no owner.
- **Placeholders.** FEA and CFD rows have no Requires and no acceptance; they should name benchmarks (NAFEMS, MMS
  convergence orders, standard CFD validation cases), peers and the cores they consume.
- **Ownership findings.** [EYLEM-v7a](../ROADMAP.md#slice-eylem-v7a) still plans an internal Jacobi-PCG "refactored to use
  crd-hesap-iterative once Phase 3.1.6 ships"; hesap's iterative solvers are shipped, so the internal solver should be
  dropped from the row (OWN.1 finding).
- **V&V and UQ** need one shared harness: MMS with symbolic sources, observed order of accuracy, grid convergence index,
  ASME V&V 10/20 reports, validation metrics, polynomial chaos and stochastic collocation, MLMC, Sobol sensitivity and
  Bayesian calibration.
- **Acausal modeling.** A Modelica-class equation language (acausal connectors, index reduction, tearing) is the
  missing half of co-simulation. Under (c) it is a CHIR dialect compiled to CEIR, using `hesap-ode` DAE support.
- **Principles.** (a) Weak forms, constitutive models, boundary conditions and solver settings are authored CHIR (the
  role UFL plays for FEniCSx); a running simulation accepts a changed material or boundary condition at the next step
  with state carried over, and a changed form recompiles its kernels in the background. (b) Every linear, nonlinear,
  eigen and time-integration solve calls hesap. (e) Matrix-free GPU kernels have a CPU path; the SSE2 floor applies.
  (f) Peers: PETSc/hypre/MUMPS for solvers, MFEM and deal.II for FE throughput, OpenFOAM and SU2 for CFD, cuDSS for GPU
  direct; accuracy by MMS and benchmark suites before speed.

<a id="area-4-cad"></a>
## 4. CAD and B-rep

### State of the art 2026

- **Kernels.** Open CASCADE Technology 8.0.0 (2026-05-07) and 8.0.1 (2026-07-30): C++17 minimum, a redesigned geometry
  evaluation, a graph-based B-rep representation (`BRepGraph`) with history tracking, smaller STEP output
  [B29](#b29). Parasolid (Siemens) and ACIS (Spatial) remain the commercial kernels (K). Open research kernels exist
  (Fornjot, Truck) but are far from production (K, status uncertain).
- **Topological naming.** FreeCAD 1.0 (November 2024) merged a topological-naming mitigation (K); FreeCAD 1.1
  (2026-03-25) focused on quality, previews and FEM [B30](#b30).
- **Exchange.** ISO 10303-242 edition 4 (AP242, August 2025) adds datum-set and tolerance-modifier PMI; edition 3 (2022)
  was withdrawn [B31](#b31). IGES 5.3, JT (ISO 14306) and 3MF remain in use (K).
- **Constraint solvers.** Siemens D-Cubed 2D/3D DCM is the commercial standard; SolveSpace and FreeCAD's planegcs are
  open references (K).
- **Surfaces.** NURBS (Piegl and Tiller), subdivision surfaces through OpenSubdiv, T-splines (K).
- **PCB.** KiCad 10.0.0 (2026-03-20): time-domain track tuning, graphical rule editor, design variants, Allegro/PADS
  import [B32](#b32).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Tolerant B-rep kernel | Parasolid, ACIS, OCCT 8.0 [B29](#b29) | Later: [BREP.1](../ROADMAP.md#slice-brep.1), [BREP.2](../ROADMAP.md#slice-brep.2) |
| Robust predicates and exact Booleans | CGAL, EMBER (Part 1 area 11) | Missing; Part 1 proposes GEOM-K and GEOM-EXACT |
| NURBS evaluation | OCCT, SISL (K) | Partial: `hesap-motion` NURBS curves and geometry curves; no surfaces or trimming |
| Subdivision surfaces | OpenSubdiv (K) | Planned in [MODEL.1](../ROADMAP.md#slice-model.1) |
| Feature tree and naming | FreeCAD 1.0/1.1, Onshape (K) [B30](#b30) | Later: [CAD.1](../ROADMAP.md#slice-cad.1); BREP.2 names stable feature naming |
| Constraint solver | D-Cubed DCM, SolveSpace (K) | Later: [CAD.2](../ROADMAP.md#slice-cad.2) placeholder; `hesap-opt` has Levenberg-Marquardt |
| STEP AP242 ed. 4, IGES, JT | [B31](#b31) | Later: [BREP.3](../ROADMAP.md#slice-brep.3) ("STEP/IGES/other declared formats") |
| Drafting and GD&T | ASME Y14.5 (K) | Later: [CAD.3](../ROADMAP.md#slice-cad.3) |
| CAM toolpaths | Fusion/Mastercam class (K) | Later: [CAM.1](../ROADMAP.md#slice-cam.1) to [CAM.9](../ROADMAP.md#slice-cam.9) |
| PCB | KiCad 10 [B32](#b32) | Later: [EDA.1](../ROADMAP.md#slice-eda.1) to [EDA.4](../ROADMAP.md#slice-eda.4) |

### Gaps in area 4

- **The kernel depends on Part 1's geometry kernel.** BREP.2 should build on GEOM-K (filtered exact predicates) and
  GEOM-EXACT, with explicit edge/vertex/face tolerances in the OCCT/Parasolid style; "exact CAD" must stay a tolerance
  claim (G20).
- **Constraint solving** needs a real design: graph decomposition into rigid clusters, numeric solve through
  `hesap-opt`, diagnosis of under- and over-constrained sketches with the offending constraints named, drag solving,
  and 3D assembly mates.
- **Persistent naming** with a history graph (the problem FreeCAD fixed in 1.0 and OCCT 8.0's `BRepGraph` history
  supports) must be the CAD.1 design, not a later repair.
- **Exchange** pins AP242 edition 4 with semantic PMI, plus AP203/214 legacy read, IGES 5.3, JT and 3MF, with OCCT 8.0
  as the independent oracle.
- **CAM and EDA** reuse Part 1's lattice family (GEOM-K.4) and exact 2D offsetting (GEOM-EXACT.1); KiCad 10 is the
  interop peer for EDA.
- **Principles.** (a) The feature tree is a CHIR program: editing a sketch dimension re-evaluates only downstream
  features in the background, and the viewport keeps the last-good solid until the new one is valid. (b) Constraint
  and surface-intersection solves use hesap. (d) Tessellation for display lives in geometry; the kernel exposes exact
  data. (f) Peers: OCCT 8.0 for Boolean robustness and speed on a common corpus; SolveSpace for sketch solving; KiCad for
  DRC speed and output correctness.

<a id="area-5-science"></a>
## 5. Scientific computing and notebooks

### State of the art 2026

- **Environments.** MATLAB R2026a (spring 2026; Copilot features, Python environment management, web publishing of
  interactive graphics) [B34](#b34); Julia 1.12.0 (2025-10-08), 1.13 release candidate April 2026 [B35](#b35); JAX
  0.10/0.11 in 2026 (versions from a third-party tracker, uncertain) [B36](#b36); NumPy 2.x, SciPy, the Python array API
  standard, Mathematica (K).
- **Notebooks.** Jupyter Notebook 7 and JupyterLab 4 (K); reactive notebooks: Pluto.jl, marimo, Observable (K).
- **Plotting.** Matplotlib, Makie (GPU), Plotly (K).
- **Autodiff.** JAX transformations (grad, jit, vmap), Enzyme (LLVM) (K).
- **Data formats.** HDF5 2.0.0 (2025-11-10; much faster virtual datasets, CMake-only builds, new default
  `libver` lower bound) and later 2.x [B33](#b33); MAT v7.3 is HDF5-based; NetCDF-4/CF, Apache Arrow and Parquet, Zarr v3
  (K).

### Cerid comparison (crd-hesap plus CHIR-SCI as a MATLAB-core replacement)

| MATLAB core area | Cerid status |
|---|---|
| Dense and sparse linear algebra, eigen, SVD | Has (`hesap-dense`, `-sparse`, `-direct`, `-eigen`) with peer boards; GPU in [HGP-0](../ROADMAP.md#slice-hgp-0), [HGP-1](../ROADMAP.md#slice-hgp-1) |
| ODE/DAE suite (`ode45`, `ode15s`, `ode15i`) | Has (`hesap-ode`: ERK, BDF, IMEX, DAE, events) |
| Optimization Toolbox class | Has (`hesap-opt`); [HGP-2](../ROADMAP.md#slice-hgp-2) GPU |
| Signal Processing, Wavelet toolboxes | Has (`hesap-dsp`, `hesap-wavelet`, `hesap-fft`) |
| Statistics and Machine Learning | Partial (`hesap-stats`: distributions, tests, MCMC, KDE); regression/clustering in [HGP-5](../ROADMAP.md#slice-hgp-5) |
| Interpolation, quadrature, special functions | Has |
| Symbolic Math Toolbox | Missing |
| Language: arrays, indexing, broadcasting, cell/struct, tables/timetables, strings, datetime, categorical, classdef, function handles | Planned: [CHIR-SCI.1](../ROADMAP.md#slice-chir-sci.1) (Review) pins indexing/broadcast/operators; the rest unnamed |
| Plotting and graphics | Planned in three rows: [CHIR-SCI.3](../ROADMAP.md#slice-chir-sci.3), [NOTEBOOK.2](../ROADMAP.md#slice-notebook.2), [SCIVIZ.2](../ROADMAP.md#slice-sciviz.2) (Later, Order 772) |
| Live Editor / notebook | Planned: [NOTEBOOK.1](../ROADMAP.md#slice-notebook.1) to [NOTEBOOK.5](../ROADMAP.md#slice-notebook.5) |
| Data I/O (MAT, HDF5, CSV, NetCDF, Parquet) | Planned: [NOTEBOOK.3](../ROADMAP.md#slice-notebook.3) names CSV/MAT/HDF5 subsets; `hesap-tensor` has DLPack and zip I/O |
| Parallel Computing Toolbox (parfor, distributed arrays, GPU arrays) | Partial: jobs and GPU rows; distributed missing (see HESAP-DIST) |
| Simulink class | Planned/missing: [SIM](../ROADMAP.md#slice-sim) rows; acausal modeling missing |

| Other reference | Cerid status |
|---|---|
| JAX transformations | Partial: forward/reverse/graph AD and `emit_cpp`; no `vmap`-style batching transform over CHIR |
| Enzyme | Out of scope by an earlier decision (LLVM plugin, MSVC-incompatible; [v16 scoreboard](../bench/2026-07-07-v16z-scoreboard.md)) |
| Reactive notebooks | The v13 notes aim beyond Pluto/marimo (typed reproducible DAG); NOTEBOOK.1 says "execution dependencies" |
| Jupyter protocol | Missing (no kernel adapter or `.ipynb` exchange) |

### Gaps in area 5

- **Plotting order conflict.** The notebook (Order 521-526) and CHIR-SCI.3 (518) need plotting, which belongs to
  `crd-sciviz` ([SCIVIZ.2](../ROADMAP.md#slice-sciviz.2), Order 772, Later). Either the plotting core moves before
  NOTEBOOK.2 or the notebook grows its own plotting, which (d) forbids.
- **No CAS.** Symbolic differentiation, simplification, polynomial algebra and equation solving are needed by the
  MATLAB-class language, MMS source terms, Modelica-class index reduction and CAD constraint analysis.
- **Language scope is under-specified.** CHIR-SCI.1 pins indexing and operators; MATLAB parity also needs cell arrays,
  structs, tables and timetables, string arrays, datetime/duration, categorical, integer saturation, logical indexing,
  `end`, varargs, closures, classdef objects, error identifiers and an explicit 0-based/1-based and row/column-major
  decision with its migration story.
- **Data formats** need pinned subsets: MAT v5 and v7.3 (HDF5), HDF5 2.x, NetCDF-4/CF, Arrow (C data interface, zero copy)
  and Parquet, NPY/NPZ, Zarr v3.
- **Notebook execution model.** State the reactive DAG semantics (no hidden state, automatic downstream re-run, as in
  Pluto/marimo/Observable) and add a Jupyter kernel adapter and `.ipynb` import/export for interop.
- **Principles.** (a) A notebook cell, a CHIR-SCI function or a plot specification edited live re-runs only its
  dependents. (b) CHIR-SCI is a language over hesap, never a second numerics library. (c) It is the same CHIR. (e)
  Every kernel keeps the SSE2 floor; GPU arrays fall back to CPU. (f) Peers: MATLAB, NumPy/SciPy, Julia, Eigen,
  MKL/OpenBLAS (approved), plus h5py/HDF5 for I/O throughput.

<a id="area-6-web"></a>
## 6. Web applications

### State of the art 2026

- **WebAssembly 3.0** became the live standard on 2025-09-17: Memory64 (browsers cap 64-bit memories at about 16 GB),
  garbage collection, exception handling, tail calls, relaxed SIMD, multiple memories, typed references, JS string
  builtins and a deterministic profile [B37](#b37). Threads and SIMD128 shipped earlier (K).
- **JSPI** (JavaScript Promise Integration, letting synchronous Wasm code await JavaScript promises) ships in Chrome and
  Edge from 137; Safari and Firefox status differ between sources (uncertain) [B38](#b38).
- **WASI 0.3** was ratified on 2026-06-11 with native async (`stream<T>`, `future<T>`) in the component model; Wasmtime
  46 enables it by default [B39](#b39).
- **WebGPU** is in all major browsers ([Part 1 W27](2026-10-26-state-of-the-art-2026-comparison.md#w27)); Linux and
  Android coverage is still partial (Firefox Linux expected in 2026), and Chrome 146 added an opt-in compatibility mode
  on OpenGL ES 3.1 for older devices [B41](#b41).
- **Isolation.** Threads need `crossOriginIsolated`; Chrome 137 added Document-Isolation-Policy as a per-frame
  alternative to COOP/COEP [B40](#b40).
- **Storage and offline.** Origin Private File System with synchronous access handles in workers, storage quotas and
  eviction, `navigator.storage.persist()`, service workers and web app manifests (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| WebGPU runtime | WebGPU in all major browsers; compatibility mode [B41](#b41) | Partial: `kir-webgpu` WGSL emitter only; runtime in [WEB.1](../ROADMAP.md#slice-web.1) |
| Wasm threads and scheduler | Wasm threads, workers | Planned: [JOBS-PORT.3](../ROADMAP.md#slice-jobs-port.3) |
| Wasm 3.0 features | Memory64, EH, tail calls, relaxed SIMD [B37](#b37) | Missing (no hits for Memory64, JSPI, WASI in docs or source) |
| Runtime code generation | Instantiate generated Wasm modules at runtime (K) | Missing; WEB.1 says "no assumed native DLL reload" |
| Persistence and offline | OPFS, quotas, service workers (K) | Planned: [WEB.4](../ROADMAP.md#slice-web.4) |
| Accessibility | Parallel DOM/ARIA semantics for canvas UIs (K); WCAG 2.2 (G19) | Planned: [WEB.2](../ROADMAP.md#slice-web.2), [WEB.5](../ROADMAP.md#slice-web.5) |
| Deployment headers | COOP/COEP or Document-Isolation-Policy, CSP with `wasm-unsafe-eval` [B40](#b40) | Missing as acceptance |
| Server-side and plugin Wasm | WASI 0.3 components [B39](#b39) | Missing |

### Gaps in area 6

- **The web's live swap.** Native Cerid reloads DLLs; the browser cannot, but it can compile and instantiate new Wasm
  modules at runtime. CEIR programs should lower to Wasm in the browser (and run interpreted while compiling), so (a)
  holds on the web. That needs `wasm-unsafe-eval` in the CSP, a module cache and generation-safe table swaps.
- **Feature baseline and tiers.** Name the Wasm 3.0 feature set Cerid requires, the fallback when a feature is missing
  (for example, no threads without cross-origin isolation, so JOBS-PORT.3's single-thread fallback), relaxed-SIMD and
  SIMD128 tiers under SIMD-DISPATCH, JSPI for synchronous engine code that waits on asynchronous browser APIs, and a
  WebGPU compatibility tier (OpenGL ES 3.1 class) in Part 1's GPU-TIER.0 ladder.
- **Deployment** as acceptance: isolation headers, streaming compilation, compression, cache policy, content
  security policy, and `ceridc export web` producing one deployable application from one Cerid project.
- **Isolated plugins.** ADR-0130 separates trusted native plugins from isolated untrusted providers; WASI 0.3
  components are a strong candidate for the isolated tier on desktop and server as well as in the browser (EXT.3).
- **Principles.** (a) Above. (c) The same CHIR runs in the browser. (e) WebGPU compatibility mode and a no-threads path
  are the older-device fallbacks. (f) Peers: Unity and Godot web exports for load time and frame time, Figma-class canvas
  apps for UI latency (K).

<a id="area-7-networking"></a>
## 7. Networking, multiplayer and collaboration

### State of the art 2026

- **Netcode models.** Rollback (GGPO) for fighting games; server-authoritative prediction, reconciliation and lag
  compensation (Overwatch, GDC 2017; Valorant 128-tick servers) (K); Unreal Iris replication (opt-in in 2026, Epic now
  recommends it over Mass replication, Mover/Network Prediction support still arriving) [B45](#b45); deterministic
  lockstep for RTS (K). ADR-0087's rule "scale the network model to interaction locality and player count" matches the
  industry practice.
- **Transport.** QUIC (RFC 9000), HTTP/3 (RFC 9114), unreliable datagrams (RFC 9221) (K); WebTransport enabled by
  default in Safari 26.4 (March 2026), but interop is uneven and the protocol draft still changes (secondary sources,
  uncertain) [B42](#b42).
- **Anti-cheat.** Server authority first; kernel anti-cheat (EAC, BattlEye, Vanguard) with Proton support for EAC and
  BattlEye (K). Microsoft's Windows Resiliency Initiative (November 2024) is moving security products out of the kernel;
  its effect on anti-cheat is unconfirmed [B46](#b46).
- **Servers and backends.** Agones on Kubernetes for game-server fleets, matchmaking services (K).
- **Collaborative editing.** Automerge 3.0 (2025-07-14) cut memory use by over 10x with the same file format
  [B43](#b43); Loro 1.0 (2024-10-23) builds on the Eg-walker algorithm [B44](#b44); Yjs (K); Figma's server-authoritative
  multiplayer and Unreal multi-user editing ([Part 1 W60](2026-10-26-state-of-the-art-2026-comparison.md#w60)).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| Transport and security | QUIC, TLS 1.3, WebTransport [B42](#b42) | Review/Later: [NET.1](../ROADMAP.md#slice-net.1) to [NET.5](../ROADMAP.md#slice-net.5); no `crd-net` in source |
| Deterministic simulation and replay | GGPO, Box2D v3 determinism | Partial: ADR-0063 contract, DIAG.9 replay; [4.2b](../ROADMAP.md#slice-4.2b) Later |
| Rollback and prediction | GGPO, Overwatch, Valorant (K) | Later: [4.2c](../ROADMAP.md#slice-4.2c) |
| Replication, relevancy, bandwidth | Unreal Iris [B45](#b45) | Partial: [NET.3](../ROADMAP.md#slice-net.3) (deltas, budgets); interest management not named |
| Dedicated servers, fleets, backends | Agones, matchmaking (K) | Partial: [4.2d](../ROADMAP.md#slice-4.2d) lobby/session stubs |
| Anti-cheat | Server authority plus client integrity (K) [B46](#b46) | Missing as a row ([NET.4](../ROADMAP.md#slice-net.4) covers hostile traffic only) |
| Collaborative editing | Automerge 3, Yjs, Loro [B43](#b43) [B44](#b44) | Review/Later: [COLLAB.1](../ROADMAP.md#slice-collab.1) to [COLLAB.5](../ROADMAP.md#slice-collab.5) |

### Gaps in area 7

- **The netcode spectrum** (lockstep, rollback, authoritative snapshot interpolation with prediction and
  reconciliation, lag compensation, interest management and priority budgets) should be named in 4.2c as selectable,
  authored replication policies, not one netcode.
- **Dedicated servers and operations**: headless server builds without a GPU, fleet orchestration adapters, matchmaking
  and backend service adapters, region selection and autoscaling, separate from the lobby stubs.
- **Anti-cheat** has no owner: server authority and validation as the primary defence, statistical and replay-based
  detection, user-mode client integrity, optional vendor SDK adapters with Linux/Proton support, and a privacy policy.
- **CRDT choice.** COLLAB.1 should evaluate Automerge 3, Yjs and Loro as peers for text and independent properties, and
  record why graphs, topology and constrained parameters stay transactional (ADR-0130 item 4).
- **Principles.** (a) Replication rules, prediction settings and server logic are authored CEIR/CHIR; a server can take a
  new rule generation between ticks with a version handshake, and collaborative edits to CEIR graphs install live on
  every client. (b) Compression, filtering and lag-compensation math use hesap. (e) Browser clients use WebTransport
  with a WebSocket fallback. (f) Peers: GGPO for rollback cost, Unreal Iris or Netcode for Entities for bandwidth per
  player, Automerge/Yjs/Loro for merge cost and memory.

<a id="area-8-robotics"></a>
## 8. Robotics, aerospace, autonomy and embedded

### State of the art 2026

- **ROS 2.** Lyrical Luth (May 2026) is the twelfth distribution and an LTS supported to May 2031, with Ubuntu 26.04 and
  Windows 11 as Tier 1 platforms; Fast DDS is the default RMW, and Zenoh has been Tier 1 since Kilted Kaiju (May 2025)
  [B47](#b47) [B48](#b48). Jazzy is the previous LTS (K).
- **Simulators.** Isaac Sim 5.0 (generally available August 2025) open-sourced its Sim-specific code under Apache 2.0
  (Omniverse Kit stays closed), with ROS 2 Jazzy and the ROS 2 simulation-interfaces standard [B49](#b49); Gazebo Jetty
  (2025, K), CARLA 0.10 on Unreal 5 (K), MuJoCo and Newton (Part 1).
- **Astrodynamics.** NAIF SPICE Toolkit N0067 (2022-01-03) is still the current version; the leap-second file stays
  valid to at least 2026-07-01 [B50](#b50); JPL DE440, SGP4, GMAT and Orekit (K).
- **Autopilots.** PX4 and ArduPilot with MAVLink 2 and SITL/HITL; PX4 uses uXRCE-DDS to ROS 2 (K).
- **Safety and embedded.** MISRA C:2025 (March 2025, 225 guidelines) [B51](#b51); MISRA C++:2023 merges AUTOSAR C++14
  [B51](#b51); DO-178C with DO-330/331/332/333, ISO 26262, IEC 61508; Simulink Embedded Coder and SCADE generate certified
  code (K).

### Cerid comparison

| Capability | Best 2026 reference | Cerid status |
|---|---|---|
| ROS 2 bridge | ROS 2 Lyrical, Zenoh, simulation interfaces [B47](#b47) [B49](#b49) | Later, duplicated: [4.2e](../ROADMAP.md#slice-4.2e) and [8.0c](../ROADMAP.md#slice-8.0c) |
| Robot description import | URDF, SDFormat, MJCF, USD (K) | Later, duplicated: [8.0a](../ROADMAP.md#slice-8.0a) and [EYLEM-v6c-urdf](../ROADMAP.md#slice-eylem-v6c-urdf) |
| Sensor simulation | Isaac Sim RTX sensors [B49](#b49); CARLA, Gazebo (K) | Later: [SENSOR.1](../ROADMAP.md#slice-sensor.1) to [SENSOR.3](../ROADMAP.md#slice-sensor.3), [EYLEM-v6e-sensor-b](../ROADMAP.md#slice-eylem-v6e-sensor-b) |
| Scenarios | OpenSCENARIO, OpenDRIVE (K) | Later: [AUTONOMY.1](../ROADMAP.md#slice-autonomy.1) |
| Estimation, control, MPC | acados, CasADi, Drake (K) | Later: CONTROL.1-13 placeholders (two "Path planning" rows, generic "Estimation"/"Control") |
| Flight model and aero | JSBSim (K) | Later, duplicated: [8.1a](../ROADMAP.md#slice-8.1a) and [EYLEM-v6f-aero-b](../ROADMAP.md#slice-eylem-v6f-aero-b) |
| SPICE and orbits | SPICE N0067, Orekit, GMAT [B50](#b50) | Later: [3.1.19](../ROADMAP.md#slice-3.1.19); time scales in [SCI-CONTRACT.3](../ROADMAP.md#slice-sci-contract.3) |
| Autopilot interop | PX4/ArduPilot MAVLink SITL/HITL (K) | Missing (no hits) |
| Embedded code generation | Embedded Coder, SCADE (K); MISRA [B51](#b51) | Later: [EMBED.1](../ROADMAP.md#slice-embed.1) to [EMBED.3](../ROADMAP.md#slice-embed.3); hesap already allocation-free and deterministic |
| Lie groups | Sophus, manif (K) | Later: [8.2a](../ROADMAP.md#slice-8.2a) in crd-math (fixed-size, correct owner) |
| Autodiff for control | CasADi, JAX (K) | Misplaced: [8.2b](../ROADMAP.md#slice-8.2b) puts autodiff in crd-math; `hesap-autodiff` exists |

### Gaps in area 8

- **Duplicates.** One ROS 2 bridge (keep 4.2e, fold 8.0c), one robot-description importer set (keep the EYLEM-v6c
  importers, 8.0a consumes them), one aero and atmosphere model (keep EYLEM-v6f-aero, 8.1a consumes it), path planning in
  Part 1's NAV rows plus CONTROL.8 trajectory optimization, and autodiff in `hesap-autodiff` (8.2b retired into it).
- **ROS 2 detail.** Target Lyrical (LTS) and Jazzy; choose native RMW participation (Zenoh or DDS) versus a bridge
  process; generate type support from `.msg`/`.idl` into Cerid reflection; implement the simulation-interfaces standard.
- **Sensor peers** for SENSOR.2: Isaac Sim, Gazebo and CARLA; lidar beam patterns, multi-return and motion distortion;
  radar cross-section, Doppler and multipath; camera ISP noise, rolling shutter, lens distortion and HDR on the shared
  renderer.
- **Autopilot interop** (MAVLink 2 SITL/HITL with PX4 and ArduPilot, uXRCE-DDS) has no row.
- **SPICE**: own readers for SPK, CK, PCK, FK, LSK and SCLK kernels with CSPICE as the oracle, DE440 ephemerides, SGP4
  for TLEs and high-fidelity propagation on `hesap-ode`.
- **Embedded evidence.** EMBED.2 should target MISRA C:2025 and MISRA C++:2023 for generated code, with static-analysis
  evidence, WCET analysis and DO-178C/DO-331 and ISO 26262 traceability as separately qualified capabilities; no
  certification claim follows from a benchmark (ADR-0130).
- **Principles.** (a) Controllers, estimators, sensor models and scenarios are authored CEIR/CHIR; a controller gain or
  model can change during a running simulation, while deployed embedded code is regenerated, never hot-swapped
  (EMBED.3 keeps physical actuation separate). (b) Estimation, MPC and trajectory optimization use `hesap-opt`,
  `hesap-ode` and `hesap-autodiff`. (d) Lie groups stay in crd-math as fixed-size types. (e) Embedded targets include
  NEON and small cores without an FPU (fixed point, matching EYLEM-v9c). (f) Peers: acados/CasADi for MPC solve time,
  Orekit/GMAT for propagation accuracy, Isaac Sim/Gazebo for sensor throughput.

<a id="gaps-and-proposed-roadmap-changes"></a>
## Gaps and proposed roadmap changes

Proposals only; the user approves before any [ROADMAP](../ROADMAP.md) edit. "After" names the existing row and its Order
number (as of 2026-10-26) that the new row should follow; the validator's order rule means a row's Requires must sit
earlier in the table. IDs from Part 1 that are themselves proposals are marked "(P1)". Highest impact first, matching
the ranking above. No retained ID is removed: a folded row keeps its ID and points to its owner.

| # | Proposed ID | Kind | Placement | Requires | Content and acceptance |
|---:|---|---|---|---|---|
| 1 | OWN.1 | Amend | In place (102) | HESAP-JOBS-RETUNE | Registry covers domain capabilities: autodiff (`hesap-autodiff`), audio DSP (`hesap-dsp`), meshing (geometry), FE/FV cores, ROS 2 bridge, robot-description import, aero/atmosphere, plotting (`crd-sciviz`); the audit lists each duplicate row below |
| 1 | 8.2b | Amend | In place (827) | OWN.1 | Retire into HGP-4 and `hesap-autodiff`; crd-math keeps only fixed-size dual types where a consumer needs them |
| 1 | 8.0c, 8.0a, 8.1a | Amend | In place | OWN.1 | 8.0c folds into 4.2e; 8.0a consumes EYLEM-v6c-urdf/sdf/mjcf; 8.1a consumes EYLEM-v6f-aero-a to -d |
| 1 | CONTROL.10 to CONTROL.13 | Amend | In place | OWN.1 | Path planning moves to NAV.3 (P1) and CONTROL.8; CONTROL.11/12 are reconciled with CONTROL.1-9 or given distinct scope |
| 1 | EYLEM-v7a | Amend | In place (685) | OWN.1 | Drop the internal Jacobi-PCG; use `hesap-iterative` and `hesap-direct` from the start |
| 2 | SCIVIZ-PLOT | New | After [CHIR-SCI.2](../ROADMAP.md#slice-chir-sci.2) (517) | CHIR-SCI.2, OWN.1 | Plotting core in `crd-sciviz`: 2D/3D plots, colormaps (SCIVIZ.3 content), interactive GPU rendering through the frame graph, SVG/PDF/PNG export headless; MATLAB/Matplotlib/Makie parity corpus; CHIR-SCI.3, NOTEBOOK.2, EYL-2 and audio meters consume it; SCIVIZ.2/.3 close against it |
| 3 | HESAP-DIST | New | After [HGP-6](../ROADMAP.md#slice-hgp-6) (713) | HGP-1, JOBS-HARD | Owned communication interface (MPI adapter, shared-memory and multi-GPU transports), distributed vectors/sparse matrices, ghost exchange, graph partitioning, parallel AMG, BDDC/FETI-DP and Schwarz, distributed sparse direct; peers PETSc, hypre, MUMPS, cuDSS MGMN; 6.4 consumes it |
| 4 | MESH-SIM.1 | New | After [BREP](../ROADMAP.md#slice-brep) (717) | BREP.2, GEOM-K.3 (P1) | Analysis meshing: CAD- and discrete-surface meshing, Delaunay/advancing-front tetrahedra, size fields, quality metrics; peers Gmsh 4.15, Netgen, TetGen, fTetWild |
| 4 | MESH-SIM.2 | New | After MESH-SIM.1 | MESH-SIM.1 | Boundary-layer prisms, hex and hex-dominant meshing, curved high-order elements |
| 4 | MESH-SIM.3 | New | After MESH-SIM.2 | MESH-SIM.1, HESAP-DIST | Adaptive h/p refinement and coarsening, parallel partitioned meshes, solution transfer |
| 4 | FE-CORE | New | After MESH-SIM.3 | MESH-SIM.1, HESAP-DIST | Finite-element core: H1/H(curl)/H(div)/L2 spaces to high order, CHIR-authored weak forms, sparse and matrix-free GPU assembly with CPU fallback, BCs; MMS convergence orders; peers MFEM, deal.II, FEniCSx |
| 4 | FV-CORE | New | After FE-CORE | MESH-SIM.1, HESAP-DIST | Finite-volume core: polyhedral cells, fluxes, gradient reconstruction, limiters, pressure-velocity coupling; peers OpenFOAM, SU2 |
| 4 | CFD.1 to CFD.4, FEA.1 to FEA.6 | Amend | In place | FV-CORE / FE-CORE, VV-UQ | Replace placeholders with acceptance: NAFEMS and standard CFD validation cases, MMS orders, adjoints (SU2-class), contact and nonlinear materials (Abaqus-class), peers named; FEA.6 owns partitioned and monolithic multiphysics coupling |
| 5 | AUDIO-DRV | New | After [PLG-1](../ROADMAP.md#slice-plg-1) (555) | PLG-1, JOBS-QOS (P1) | Device layer: capture and duplex, WASAPI exclusive and `IAudioClient3`, ASIO (licence decision), native PipeWire and the JACK API, ALSA fallback, CoreAudio with MAC, aggregate devices, clock drift, hot-plug and device loss; round-trip latency board against Reaper |
| 6 | AUDIO-CEIR.1 | Amend | In place (561) | PLG-6 | The real-time path executes the authored graph; live generation swap built off the audio thread, state migration by node identity, authored crossfade for structural edits, zero dropouts in a rapid-edit soak |
| 6 | AUDIO-GAME | New | After AUDIO-SPATIAL | AUDIO-SPATIAL | Game-audio behaviour as authored CEIR: events, states, switches, RTPC, voice limits/virtualization/priority, streaming banks, ducking and side-chains, interactive music with beat-synchronous transitions, dialogue; profiler; peers Wwise 2025.1, FMOD, MetaSounds |
| 7 | AUDIO-SPATIAL | New | After [AUDIO-CEIR.4](../ROADMAP.md#slice-audio-ceir.4) (564) | AUDIO-CEIR.4, HESAP-AUDIO-DSP | Higher-order Ambisonics, SOFA (AES69) HRTF with interpolation and near-field, object/bed renderer (VBAP), ADM BS.2076 and IAMF import/export, binaural monitoring, geometric acoustics on `crd-geometry-bvh`; peer Steam Audio |
| 8 | MED-REVIEW | Amend | In place (538) | NOTEBOOK | Record two licensing postures (owned royalty-free codecs; OS/driver hardware paths for H.264/HEVC/VVC), AV2 v1.0, and the AV1/VP9 distribution-pool claim as a risk for legal review |
| 8 | MED-HW | New | After [MED-GPU-1](../ROADMAP.md#slice-med-gpu-1) (540) | MED-GPU-1, GPU-TIER.0 (P1) | Vulkan Video (H.264/H.265/AV1 decode and encode, VP9 decode, quantization maps, intra refresh), D3D12 Video and Media Foundation, VA-API; optional NVENC/AMF/oneVPL seams; zero-copy frame-graph surfaces; per-codec capability tiers with the owned software codec as fallback |
| 9 | PLG-7 | New | After [PLG-6](../ROADMAP.md#slice-plg-6) (560) | PLG-6, I2D-8 | Per-plugin out-of-process hosting with shared-memory audio and crash recovery; plugin editor windows embedded in crd-ui (Win32, X11, Wayland, Cocoa) with scale factors |
| 9 | PLG-8 | New | After PLG-7 | PLG-7 | ARA 2, LV2 and AU/AUv3 (with MAC) behind the PLG-2 facade; CLAP 1.2.x extension parity (polyphonic modulation, note expressions, thread pool, undo) |
| 9 | PLG-4 | Amend | In place (558) | PLG-3 | Record the VST 3.8 MIT licence (2025-10-29); include note expressions and the 3.8 SDK feature set |
| 10 | MIDI-UMP | New | After AUDIO-DRV | AUDIO-DRV | UMP endpoints through Windows MIDI Services, ALSA UMP (rawmidi and sequencer) and CoreMIDI; groups, function blocks, jitter-reduction timestamps, MIDI-CI and Property Exchange, MPE; MIDI 1.0 translation |
| 11 | HESAP-AUDIO-DSP | New | After [MED-REVIEW](../ROADMAP.md#slice-med-review) (538) | OWN.1 | In `hesap-dsp`: time-stretch and pitch-shift (phase-locked vocoder, transient and formant handling), loudness (BS.1770, R128) and true peak, dither and noise shaping, non-uniform partitioned convolution, oversampling; peers Rubber Band R3, Signalsmith, élastique |
| 12 | HESAP-SYM | New | After [HGP-5](../ROADMAP.md#slice-hgp-5) (515) | HGP-4 | Symbolic algebra in hesap: expression DAG shared with graph AD, simplification, polynomial algebra (Gröbner), symbolic differentiation and series, equation solving, lowering to CEIR; peers SymPy, Mathematica, Maxima |
| 12 | CHIR-SCI.1 | Amend | In place (516) | HGP-5, LANG | Pin the full MATLAB-core language scope (cell, struct, tables/timetables, strings, datetime, categorical, integer saturation, classdef, handles) and the index-base/layout decision; add a `vmap`-style batching transform to CHIR-SCI.2 |
| 13 | VV-UQ | New | After [SCI-CONTRACT](../ROADMAP.md#slice-sci-contract) (604) | SCI-CONTRACT, HESAP-SYM | Shared V&V and UQ harness: MMS with symbolic sources, observed order, grid convergence index, ASME V&V 10/20 reports, validation metrics, polynomial chaos and stochastic collocation, MLMC, Sobol, Bayesian calibration; peer Dakota |
| 14 | WEB.1 | Amend | In place (581) | MAC, JOBS-PORT | Wasm 3.0 baseline and per-feature fallbacks (Memory64, EH, tail calls, relaxed SIMD tiers under SIMD-DISPATCH, JSPI), WebGPU compatibility tier under GPU-TIER.0 (P1), isolation headers (COOP/COEP or Document-Isolation-Policy), CSP and deployment as acceptance |
| 14 | WEB-CEIR | New | After WEB.1 | WEB.1 | Runtime CEIR to Wasm code generation with an interpreter while compiling, module cache and generation-safe swap: the web's form of live replacement |
| 14 | WEB.4, EXT.3 | Amend | In place | - | WEB.4: OPFS sync handles, quotas, persistence, service worker offline, installable app; EXT.3: evaluate WASI 0.3 components as the isolated-untrusted plugin tier on desktop, server and web |
| 15 | CAD-SOLVE | New | After [CAD.2](../ROADMAP.md#slice-cad.2) (719) | BREP.2 | Geometric constraint solver: rigid-cluster decomposition, numeric solve through `hesap-opt`, under/over-constraint diagnosis, drag solving, 3D assembly mates; peers SolveSpace, planegcs (D-Cubed as a reference only) |
| 15 | CAD.1, BREP.3 | Amend | In place | BREP.2 | CAD.1: feature tree as a CHIR program with persistent naming on a history graph and background re-evaluation; BREP.3: AP242 edition 4 with semantic PMI, AP203/214, IGES 5.3, JT, 3MF; OCCT 8.0 as oracle |
| - | SIM.2 | Amend | In place (808) | SIM.1 | FMI 3.0.2 import and export (Cerid models as FMUs), SSP 2.0, FMI layered standards (LS-BUS), clocks and scheduled execution |
| - | SIM.4 | New | After [SIM.3](../ROADMAP.md#slice-sim.3) (809) | SIM.2, HESAP-SYM | Acausal equation-based modeling as a CHIR dialect: connectors, flattening, index reduction, tearing, compiled to CEIR on `hesap-ode`; OpenModelica as peer; live parameter and structure edits |
| - | 4.2c | Amend | In place (796) | 4.2b | Selectable authored replication policies: lockstep, rollback, authoritative snapshot interpolation with prediction/reconciliation, lag compensation, interest management and priority budgets |
| - | NET-SERVER | New | After [4.2d](../ROADMAP.md#slice-4.2d) (797) | 4.2d | Headless dedicated servers, fleet orchestration adapters, matchmaking and backend adapters, regions and autoscaling |
| - | ANTICHEAT | New | After [NET.4](../ROADMAP.md#slice-net.4) (793) | NET.4 | Server validation first, statistical and replay detection, user-mode client integrity, optional vendor SDK adapters with Linux/Proton support, privacy policy |
| - | COLLAB.1 | Amend | In place (801) | NET, DOC | Evaluate Automerge 3, Yjs and Loro for text and independent properties; transactional graphs/topology; live install of collaborative CEIR edits |
| - | 8.3a, 8.3b, 8.3c | Amend | In place (828-830) | - | Sequencer-class bindings, spawnables, subsequences, shots, takes, camera cuts, OTIO 0.18 interop, playback in the running game; physical camera and lens model shared with OFF; OpenJD job templates, Deadline Cloud and OpenCue adapters |
| - | VPROD | New | After [8.3b](../ROADMAP.md#slice-8.3b) (829) | 8.3b, NET.3 | Virtual production: cluster rendering for LED volumes (inner/outer frustums), genlock/framelock, OpenTrackIO tracking, lens calibration, LED colour calibration through OCIO |
| - | MED-2 | Amend | In place (544) | MED-REVIEW | EXR parity with OpenEXR 3.4: HTJ2K, deep, multipart, DWAA/B, mip/rip maps; fuzz corpus covering the 3.4.x CVE classes |
| - | INTEROP.1 | Amend | In place (839) | MODEL, BREP, MED-12 | Hydra 2.0 scene-index interop (Cerid as render delegate and as scene-index consumer), OTIO, deep EXR, MaterialX |
| - | DAW-APP.1 | Amend | In place (566) | AUDIO-CEIR | Comping, warp markers, PDC through sends and side-chains, freeze and render-in-place, clip launching, modulation routing, MPE/MIDI 2.0, ARA, LUFS/true-peak metering, ADM BWF export; peers Bitwig 6, Ableton 12, Reaper 7 |
| - | 4.2e | Amend | In place (798) | - | ROS 2 Lyrical (LTS) and Jazzy, native RMW participation (Zenoh or DDS) versus bridge, type support from `.msg`/`.idl`, simulation-interfaces standard |
| - | SENSOR.2 | Amend | In place (812) | SENSOR.1 | Peers Isaac Sim 5, Gazebo, CARLA; lidar, radar and camera-ISP model list as above |
| - | AUTOPILOT | New | After [8.1b](../ROADMAP.md#slice-8.1b) (825) | 8.1b, 4.2e | MAVLink 2 SITL/HITL with PX4 and ArduPilot, uXRCE-DDS, timing and failure injection |
| - | 3.1.19 | Amend | In place (856) | SCI-CONTRACT.3 | Own SPICE kernel readers with CSPICE as oracle, DE440, SGP4, high-fidelity propagation; peers Orekit, GMAT |
| - | EMBED.2 | Amend | In place (852) | EMBED.1 | MISRA C:2025 and MISRA C++:2023 targets for generated code, static-analysis and WCET evidence, DO-178C/DO-331 and ISO 26262 traceability as separate capabilities |

<a id="needs-a-download"></a>
## Needs a download

Not downloaded. Sizes were not checked (that would need at least a HEAD request); each item needs separate user
approval before it is fetched.

| Item | Why | URL | Size |
|---|---|---|---|
| VST 3.8 SDK | PLG-4 (now MIT) | https://github.com/steinbergmedia/vst3sdk | not checked |
| CLAP headers 1.2.x | PLG-3, PLG-8 | https://github.com/free-audio/clap | not checked |
| ARA SDK | PLG-8 | https://github.com/Celemony/ARA_SDK | not checked |
| ASIO SDK (GPLv3/proprietary) | AUDIO-DRV, licence decision | https://www.steinberg.net/developers/ | not checked |
| Windows MIDI Services SDK runtime | MIDI-UMP tests | https://aka.ms/midi (K) | not checked |
| Steam Audio SDK | AUDIO-SPATIAL peer | https://github.com/ValveSoftware/steam-audio | not checked |
| SOFA HRTF sets (for example SADIE II, CIPIC) | AUDIO-SPATIAL test data | https://www.sofaconventions.org (K) | not checked |
| IAMF tools and reference decoder | AUDIO-SPATIAL interop | https://github.com/AOMediaCodec/iamf-tools (K) | not checked |
| Rubber Band, Signalsmith Stretch | HESAP-AUDIO-DSP peers | https://breakfastquay.com/rubberband/ ; https://github.com/Signalsmith-Audio/signalsmith-stretch | not checked |
| Reaper (evaluation) | AUDIO-DRV latency peer | https://www.reaper.fm (K) | not checked |
| FFmpeg with dav1d and SVT-AV1; AV2 reference (AVM) | MED peers | https://ffmpeg.org ; https://gitlab.com/AOMediaCodec/avm (K) | not checked |
| OpenEXR 3.4.14+ and test images | MED-2 peer and corpus | https://github.com/AcademySoftwareFoundation/openexr | not checked |
| OpenTimelineIO 0.18 | 8.3a interop | https://github.com/AcademySoftwareFoundation/OpenTimelineIO | not checked |
| PETSc 3.25, hypre, MUMPS, cuDSS | HESAP-DIST peers | https://petsc.org ; https://docs.nvidia.com/cuda/cudss | not checked |
| MFEM 4.9, deal.II, FEniCSx | FE-CORE peers | https://mfem.org ; https://www.dealii.org (K) ; https://fenicsproject.org (K) | not checked |
| OpenFOAM, SU2 | FV-CORE peers | https://openfoam.org (K) ; https://su2code.github.io (K) | not checked |
| Gmsh 4.15, TetGen, fTetWild | MESH-SIM peers | https://gmsh.info | not checked |
| Dakota | VV-UQ peer | https://dakota.sandia.gov (K) | not checked |
| FMI reference FMUs, OpenModelica | SIM.2, SIM.4 | https://github.com/modelica/Reference-FMUs (K) ; https://openmodelica.org (K) | not checked |
| OCCT 8.0.1; STEP AP242 test files (CAx-IF) | BREP.3 oracle and corpus | https://dev.opencascade.org ; https://www.mbx-if.org | not checked |
| SolveSpace | CAD-SOLVE peer | https://solvespace.com (K) | not checked |
| KiCad 10 | EDA interop peer | https://www.kicad.org | not checked |
| SymPy | HESAP-SYM peer | https://www.sympy.org (K) | not checked |
| HDF5 2.x; NetCDF-C; Apache Arrow C++ | NOTEBOOK.3 formats | https://support.hdfgroup.org ; https://arrow.apache.org (K) | not checked |
| Wasmtime 46 | EXT.3 evaluation | https://github.com/bytecodealliance/wasmtime (K) | not checked |
| Automerge 3, Yjs, Loro | COLLAB.1 peers | https://automerge.org ; https://loro.dev | not checked |
| ROS 2 Lyrical | 4.2e | https://docs.ros.org/en/lyrical/ | not checked |
| Isaac Sim 5, Gazebo, CARLA 0.10 | SENSOR.2 peers | https://github.com/isaac-sim/IsaacSim (K) | not checked |
| CSPICE N0067 and DE440 kernels | 3.1.19 oracle | https://naif.jpl.nasa.gov/naif/toolkit.html | not checked |
| PX4 and ArduPilot SITL | AUTOPILOT | https://px4.io (K) ; https://ardupilot.org (K) | not checked |
| MISRA C:2025, MISRA C++:2023 documents (purchase) | EMBED.2 | https://misra.org.uk | not checked |

<a id="sources"></a>
## Sources

Read 2026-10-26 unless marked (K). Dates are publication dates as shown by the source or its coverage.

<a id="b01"></a>B01. Steinberg, [Press release: VST 3.8](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf), 2025-10-29; Sonicstate, [VST 3 now available under MIT license](https://sonicstate.com/news/2025/10/30/vst-3-now-available-under-mit-license), 2025-10-30 (ASIO GPLv3 per coverage).

<a id="b02"></a>B02. free-audio, [CLAP ChangeLog](https://github.com/free-audio/clap/blob/main/ChangeLog.md) (fetched; 1.2.7 to 1.2.10 without dates); [Wikipedia: CLever Audio Plug-in](https://en.wikipedia.org/wiki/CLever_Audio_Plug-in) (1.2.6, 2025-03-11).

<a id="b03"></a>B03. Synthtopia, [Celemony releases ARA SDK under open source license](https://www.synthtopia.com/content/2021/05/13/celemony-releases-ara-sdk-under-open-source-license/), 2021-05-13 (Apache 2.0).

<a id="b04"></a>B04. Microsoft, [Making music with MIDI just got a real boost in Windows 11](https://blogs.windows.com/windowsexperience/2026/02/17/making-music-with-midi-just-got-a-real-boost-in-windows-11/), 2026-02-17; Heise, [Windows: MIDI 2.0 has arrived](https://heise.de/-11181074), 2026.

<a id="b05"></a>B05. Linux kernel, [MIDI 2.0 on Linux](https://docs.kernel.org/6.15/sound/designs/midi-2.0.html) (design text unchanged from 6.5 to 6.19).

<a id="b06"></a>B06. Phoronix, [Valve makes all Steam Audio SDK source code available under Apache 2.0](https://phoronix.com/news/Steam-Audio-SDK-Fully-Open), 2024-02-20; Valve, [Steamworks: Steam Audio](https://partner.steamgames.com/doc/features/steam_audio) (SOFA HRTFs).

<a id="b07"></a>B07. Google Open Source, [Introducing Eclipsa Audio](https://opensource.googleblog.com/2025/01/introducing-eclipsa-audio-immersive-audio-for-everyone.html), January 2025; [Open source DAW plugin for Eclipsa Audio](https://opensource.googleblog.com/2025/06/introducing-open-source-daw-plugin-for-eclipsa-audio.html), June 2025.

<a id="b08"></a>B08. C. Cannam, [Rubber Band Library v3.0.0 released](https://lists.linuxaudio.org/archives/linux-audio-dev/2022-July/038393.html), July 2022; Breakfast Quay, [news](https://breakfastquay.com/news.html) (4.0.0, 2024-10-25).

<a id="b09"></a>B09. Qt, [Signalsmith Stretch attribution](https://doc.qt.io/QT-6/qtmultimedia-attribution-signalsmith-stretch.html) (MIT, version 1.0.0).

<a id="b10"></a>B10. Audiokinetic, [Wwise 2025.1: What's New](https://www.audiokinetic.com/en/blog/wwise-2025.1-whats-new), 2025.

<a id="b11"></a>B11. Sound On Sound, [Bitwig Studio 6 launches](https://www.soundonsound.com/news/bitwig-studio-6-launches), March 2026 (release day differs between sources); Synth Anatomy, [Bitwig Studio 6](https://synthanatomy.com/2026/03/bitwig-studio-6.html).

<a id="b12"></a>B12. Linuxiac, [PipeWire 1.6.4 released with JACK, Bluetooth and ALSA fixes](https://linuxiac.com/pipewire-1-6-4-multimedia-framework-released/), 2026 (date uncertain).

<a id="b13"></a>B13. Khronos, [Vulkan Video encode AV1 and encode quantization map extensions](https://www.khronos.org/blog/khronos-announces-vulkan-video-encode-av1-encode-quantization-map-extensions), November 2024.

<a id="b14"></a>B14. Khronos, [Khronos announces Vulkan Video decode VP9 extension](https://www.khronos.org/blog/khronos-announces-vulkan-video-decode-vp9-extension), June 2025; RasterGrid, [Vulkan Video timeline](https://www.rastergrid.com/vulkan-video/) (intra refresh, July 2025).

<a id="b15"></a>B15. AOMedia, [Alliance for Open Media releases AV2 codec](https://aomedia.org/press%20releases/Alliance-for-Open-Media-Releases-AV2-Codec/), 2026-06-09; Phoronix, [AV2 v1.0 specification released](https://phoronix.com/news/AV2-1.0-Specification-Released), 2026-05-31.

<a id="b16"></a>B16. Access Advance via BusinessWire, [Access Advance extends HEVC Advance rate increase deadline](https://www.businesswire.com/news/home/20260126799824/en/Access-Advance-Extends-HEVC-Advance-Rate-Increase-Deadline), 2026-01-26; [New licensors to the Video Distribution Patent Pool](https://www.businesswire.com/news/home/20260706502038/en/Access-Advance-Welcomes-New-Licensors-to-the-Video-Distribution-Patent-Pool), 2026-07-06 (vendor press releases).

<a id="b17"></a>B17. A. Pranckevičius, [Lossless float image compression](https://aras-p.info/blog/2025/07/08/Lossless-Float-Image-Compression/), 2025-07-08 (HTJ2K in OpenEXR 3.4); Julia security, [JLSEC-2026-813: HTJ2K out-of-bounds reads](https://security.julialang.org/advisories/JLSEC-2026-813/), 2026.

<a id="b18"></a>B18. piwheels, [opentimelineio](https://www.piwheels.org/project/opentimelineio) (0.18.0 2025-11-06, 0.18.1 2025-11-09).

<a id="b19"></a>B19. AWS, [Open Job Description open specification for portable render jobs](https://aws.amazon.com/about-aws/whats-new/2024/01/open-job-description-open-specification-portable-render-jobs), 2024-01-11.

<a id="b20"></a>B20. SMPTE RIS, [OpenTrackIO documentation v1.0.1](https://ris-pub.smpte.org/ris-osvp-metadata-camdkit/); Mo-Sys, [SMPTE OpenTrackIO launch](https://www.mo-sys.com/news/smpte), 2024-10-29.

<a id="b21"></a>B21. PETSc developers, [PETSc 3.24 release is out](https://mailman.cels.anl.gov/archives/list/petsc-dev@lists.mcs.anl.gov/thread/Z3JW3HNE3ZUKYW77BAOKOCPZZ2HOGNRH/), 2025; PETSc 3.25.0 announcement on the same list, 2026 (date uncertain).

<a id="b22"></a>B22. NVIDIA, [cuDSS documentation](https://docs.nvidia.com/cuda/cudss/latest/index.html) and [release notes](https://docs.nvidia.com/cuda/archive/13.1.0/cudss/doc_output/release_notes.html) (0.7.0 single-node multi-GPU; MGMN with a user communication layer).

<a id="b23"></a>B23. [MFEM](https://mfem.org) news (4.8 2025-09-18, 4.9 2025-12-11); Wikipedia lists 4.10 (2026-09-01, uncertain).

<a id="b24"></a>B24. Modelica Association, [FMI 2.0.5 and FMI 3.0.2 released](https://fmi-standard.org/news/2024-11-27-fmi-2.0.5-and-3.0.2), 2024-11-27; [FMI news](https://fmi-standard.org/news/) (LS-BUS 1.0 2025-07-21; 2026 layered-standard betas).

<a id="b25"></a>B25. Modelica Association, [SSP 2.0 released](https://ssp-standard.org/news/2024-12-20-ssp-2-0-release/), 2024-12-20.

<a id="b26"></a>B26. ANSI webstore, [ASME V&V 10-2019 (R2025)](https://webstore.ansi.org/standards/asme/asme102019r2025).

<a id="b27"></a>B27. [Gmsh changelog](https://gmsh.info/CHANGELOG.txt) (4.14.0 2025-07-03, 4.14.1 2025-09-02, 4.15.0 2025-10-26).

<a id="b28"></a>B28. K. Aycock, N. Rebelo, B. Craven, [Method of manufactured solutions code verification of elastostatic solid mechanics problems in a commercial finite element solver](https://ar5iv.labs.arxiv.org/html/1902.07608), arXiv 1902.07608, 2019.

<a id="b29"></a>B29. Open Cascade, [Open CASCADE Technology 8.0.0 release](https://dev.opencascade.org/content/open-cascade-technology-800-release), 2026-05-07; [Wikipedia](https://en.wikipedia.org/wiki/Open_Cascade_Technology) (8.0.1, 2026-07-30).

<a id="b30"></a>B30. FreeCAD, [FreeCAD version 1.1 released](https://blog.freecad.org/2026/03/25/freecad-version-1-1-released/), 2026-03-25. FreeCAD 1.0's topological-naming work (November 2024) is (K).

<a id="b31"></a>B31. STEP Tools, [AP242 edition 4 notes](https://steptools.com/docs/stp_aim/notes_ap242e4.html) (published August 2025); MBx-IF, [XSD schemas](https://www.mbx-if.org/home/mbx/resources/xsd-schemas/).

<a id="b32"></a>B32. KiCad, [Version 10.0.0 released](https://www.kicad.org/blog/2026/03/Version-10.0.0-Released/), 2026-03-20.

<a id="b33"></a>B33. HDF Group, [HDF5 2.0 release specifics](https://support.hdfgroup.org/documentation/hdf5/latest/rel_spec_20.html) and [2.0.0 downloads](https://support.hdfgroup.org/releases/hdf5/v2_0/v2_0_0/downloads/), 2025-11-10/11.

<a id="b34"></a>B34. MathWorks, [MATLAB R2026a has been released](https://blogs.mathworks.com/matlab/2026/04/21/matlab-r2026a-has-been-released-whats-new/), 2026-04-21.

<a id="b35"></a>B35. Julia, [Julia 1.12 highlights](https://julialang.org/blog/2025/10/julia-1.12-highlights/), October 2025 (K, not read); [Discourse release announcements](https://discourse.julialang.org/tag/release).

<a id="b36"></a>B36. GitClear tracker, [jax-v0.10.2](https://www.gitclear.com/open_repos/google/jax/release/jax-v0.10.2), 2026 (third-party, uncertain).

<a id="b37"></a>B37. WebAssembly CG, [Wasm 3.0 completed](https://webassembly.org/news/2025-09-17-wasm-3.0), 2025-09-17; SD Times, [Wasm 3.0 standard is now officially complete](https://sdtimes.com/softwaredev/wasm-3-0-standard-is-now-officially-complete/).

<a id="b38"></a>B38. Can I use, [JavaScript Promise Integration](https://caniuse.com/mdn-webassembly_jspi) (Chrome/Edge 137+; Firefox and Safari entries conflict with other sources).

<a id="b39"></a>B39. Bytecode Alliance, [WASI 0.3](https://bytecodealliance.org/articles/WASI-0.3); [wasi.dev WASI P3](https://wasi.dev/releases/wasi-p3), 2026-06-11.

<a id="b40"></a>B40. Chrome for Developers, [Document Isolation Policy](https://developer.chrome.com/blog/document-isolation-policy), 2025-05-01.

<a id="b41"></a>B41. GPU for the Web, [Implementation status](https://github.com/gpuweb/gpuweb/wiki/Implementation-Status) (edited 2026-05-28).

<a id="b42"></a>B42. Utsubo, [Frontier Web APIs in 2026](https://www.utsubo.com/blog/frontier-web-apis-2026-production-ready), 2026 (secondary; Safari 26.4 WebTransport).

<a id="b43"></a>B43. Automerge, [Automerge 3.0](https://automerge.org/blog/automerge-3/), 2025-07-14.

<a id="b44"></a>B44. Loro, [Loro 1.0](https://loro.dev/blog/v1.0), 2024-10-23; J. Gentle and M. Kleppmann, Eg-walker paper (September 2024 preprint; venue K).

<a id="b45"></a>B45. Epic, [Iris replication system](https://dev.epicgames.com/documentation/en-us/unreal-engine/iris-replication-system-in-unreal-engine); Unreal forums, [Mass replication regressions in 5.8.1](https://forums.unrealengine.com/t/three-mass-replication-regressions-in-ue-5-8-1/2743854), 2026.

<a id="b46"></a>B46. Help Net Security, [Microsoft plans to limit security products' access to Windows kernel mode](https://www.helpnetsecurity.com/2024/11/19/windows-kernel-security-vendors/), 2024-11-19.

<a id="b47"></a>B47. Open Robotics, [Release Lyrical Luth](https://docs.ros.org/en/lyrical/Releases/Release-Lyrical-Luth.html), May 2026.

<a id="b48"></a>B48. Open Robotics, [ROS 2 Kilted Kaiju released](https://www.openrobotics.org/blog/2025/5/23/ros-2-kilted-kaiju-released), 2025-05-23.

<a id="b49"></a>B49. NVIDIA, [Isaac Sim 5.0 release notes](https://docs.isaacsim.omniverse.nvidia.com/5.0.0/overview/release_notes.html); [general availability of Isaac Sim 5.0 and Isaac Lab 2.2](https://developer.nvidia.com/blog/announcing-general-availability-for-nvidia-isaac-sim-5-0-and-nvidia-isaac-lab-2-2), August 2025.

<a id="b50"></a>B50. NASA NAIF, [SPICE Toolkit](https://naif.jpl.nasa.gov/naif/toolkit) (N0067, 2022-01-03) and [announcements](https://naif.jpl.nasa.gov/naif/announcements.html).

<a id="b51"></a>B51. MISRA, [FAQ](https://misra.org.uk/?p=172) (MISRA C:2025, March 2025); Perforce, [MISRA C++:2023 introduction](https://www.perforce.com/blog/qac/misra-cpp-2023-intro).

Prior-knowledge (K) references not re-read today: WASAPI `IAudioClient3`; CoreAudio and CoreMIDI UMP; AUv3; LV2; JACK2;
ITU-R BS.2076 (ADM), BS.2127, BS.1770; EBU R128; AES69 SOFA; MPEG-H 3D Audio; Dolby Atmos; zplane élastique; FMOD
Studio; Unreal MetaSounds; Ableton Live 12; Reaper 7; Logic Pro; Studio One; D3D12 Video; Media Foundation; VA-API;
NVENC/NVDEC; AMD AMF; Intel oneVPL; AOMedia royalty-free licence; DaVinci Resolve; Nuke; Unreal Sequencer, Movie Render
Graph and nDisplay; SMPTE ST 2110; Hydra 2.0; OpenCue; Deadline 10; OpenFOAM; SU2; deal.II; FEniCSx/UFL; MOOSE;
Trilinos; hypre; MUMPS; SuiteSparse; AmgX; MPI 4.1/5.0; Netgen; TetGen; fTetWild; ASME V&V 20; Dakota; UQLab;
OpenModelica; Dymola; Ansys, Abaqus, COMSOL; Parasolid; ACIS; Fornjot; Truck; OpenSubdiv; T-splines; IGES 5.3; JT
(ISO 14306); 3MF; D-Cubed DCM; SolveSpace; planegcs; FreeCAD 1.0; NumPy; SciPy; Python array API standard; Mathematica;
Jupyter Notebook 7; JupyterLab 4; Pluto.jl; marimo; Observable; Matplotlib; Makie; Plotly; JAX transformations; Enzyme;
MAT v7.3; NetCDF-4/CF; Apache Arrow; Parquet; Zarr v3; OPFS; storage quotas; service workers; GGPO; Overwatch (GDC 2017)
and Valorant netcode; QUIC RFC 9000, HTTP/3 RFC 9114, datagrams RFC 9221; Agones; EAC and BattlEye on Proton; Yjs;
Figma multiplayer; ROS 2 Jazzy; Gazebo Jetty; CARLA 0.10; JPL DE440; SGP4; GMAT; Orekit; PX4; ArduPilot; MAVLink 2;
uXRCE-DDS; DO-178C and DO-330 to DO-333; ISO 26262; IEC 61508; Simulink Embedded Coder; SCADE; acados; CasADi; Drake;
JSBSim; Sophus; manif. Re-pin each before use.
