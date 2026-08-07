# Research — 2026-07-23 — the offline renderer frontier (the OFF band master survey)

> **Status:** **research → planned** — the D-007 OFF band tracks this survey; not yet started. *(stamped 2026-08-07, doc-hygiene pass)*

> The kickoff/index research for the D-007 **OFF band** (offline render mode, rows 46-54). One master survey mapping the
> *newest* (2024-2026 SIGGRAPH/TOG) advances onto the already-planned pillars, plus Cerid-specific recommendations. Per-pillar
> deep dives (each its own dated doc) branch from here — flagged in **Next research**.

## Question

We are building a gold-standard, film-quality **offline renderer** as a *switchable mode* on the existing CKIR integrators
(RT-3 megakernel PT, RT-4 NEE+MIS, RT-5/7 ReSTIR DI/GI, the B9/C3 AS tier, B18 hair, B14 GI). The OFF band (D-007 rows 46-54)
already names the seminal papers per pillar. This survey asks: **what is the 2024-2026 state of the art for each pillar, what
changed since the seminal work, and how should Cerid sequence + architect the OFF band** to be as good as Cycles / Arnold GPU /
RenderMan XPU / Hyperion — and serve as the *ground-truth reference* every real-time approximation is measured against.

## TL;DR

- **The offline mode is not a new renderer — it is convergence + robustness + AOVs on the integrators we already have.** The
  single highest-leverage modern result: **GRIS/ReSTIR PT (Lin 2022) can be modified to GUARANTEE unbiased convergence for
  offline renderers** — so our existing ReSTIR (RT-5/7) becomes the *offline* GI base, not a separate BDPT rewrite. Start there.
- **Path guiding is the production consensus** (Cycles, Hyperion, V-Ray, Corona, Karma all ship it) and 2024-2025 solved its two
  Cerid-blockers: **GPU/wavefront-friendly guiding** (Derin 2024) and **neural online guiding** (Huang 2024, NASG). Guiding is
  OFF-4 but is the *biggest* real-world convergence win — promote it.
- **The rest of the band is well-scoped by classical papers + a thin layer of 2024-2026 refinements:** spectral (hero-wavelength
  Wilkie 2014 + Jakob-Hanika 2019 upsampling), volumetrics (null-scattering + progressive null-tracking 2023, path graphs 2024),
  random-walk SSS (Chiang 2016) now joined by ReSTIR SSS (2024), and an **OIDN-2-class AOV CNN denoiser** (albedo+normal guide)
  as the OFF finishing filter. Architecture: **wavefront** (PBRT4 ch.15) for the offline mode, megakernel stays for real-time.

## Recommendation for Cerid

**Sequence the OFF band by convergence-leverage, not by the classical chronology.** The rows are numbered OFF-1..9 but the order
that gets us a *usable, crushing* offline renderer fastest is:

1. **OFF-1 first, but built AROUND ReSTIR-as-offline (GRIS convergence mode), not a fresh integrator.** The mode seam =
   wavefront-scheduled, tiled/bucketed, adaptive-sampled convergence of the *existing* RT-5/7 ReSTIR PT/GI with the GRIS
   convergence guarantee flipped on (unbiased in the limit) + full AOV output. This reuses everything and immediately produces a
   reference. Wavefront (per-material kernels, converged launches — PBRT4 §15) is the right GPU architecture here; keep the
   real-time megakernel for the interactive mode. This is the ADR: **offline = wavefront convergence mode over ReSTIR PT.**
2. **OFF-4 path guiding next** (promoted above BDPT/VCM/MLT) — it is the single biggest convergence multiplier in production and
   the 2024 wavefront/neural results make it GPU-viable. Order-of-magnitude fewer spp on hard indirect/caustic-ish transport with
   no precompute. This is what makes "driven to convergence" *tractable*.
3. **OFF-6 spectral + OFF-7 volumetrics/SSS** — these are correctness/quality features the domains demand (science/aerospace/film
   need spectral; skin/fur/clouds need reference volumetrics). They slot onto the wavefront integrator as sampling/BSDF changes.
4. **OFF-2/3/5 (BDPT / VCM / MLT) as the robustness tail** — the estimators that resolve SDS caustics (glass/water/gems) and
   pathological visibility that even guided PT struggles with. These are the "last 5% of scenes" — real, but lower-frequency
   need than guiding. **ReSTIR BDPT (2025)** is the modern bridge: it folds BDPT-with-caustics into the resampling framework we
   already use, so OFF-2/3 may become ReSTIR-BDPT rather than classical Veach BDPT.
5. **OFF-8 robust estimators + OFF-9 film output (deep EXR AOVs, cryptomatte, LPE)** throughout — firefly/outlier rejection and
   AOVs are needed from OFF-1 onward, not bolted on last.

**Everything stays CKIR + bit-exact-oracle-certified.** The offline mode is *also* the reference that certifies SVGF/ReSTIR/NRC
as "matched accuracy" — so the convergence path must be an unbiased estimator we can drive arbitrarily far, and every offline
kernel keeps a CPU oracle. Denoising is a *finishing* step (OIDN-2-class AOV CNN), never in the reference path.

## The frontier, pillar by pillar (seminal → newest → Cerid note)

### Architecture (OFF-1) — wavefront convergence mode
- **Classical:** megakernel (one big kernel/path) vs **wavefront** (per-stage/per-material kernels, each launch starts converged
  — PBRT4 §15). Wavefront wins with many BSDFs/lights (our case) by killing divergence.
- **2024-2026:** *Megakernel vs Wavefront GPU Path Tracing* (arXiv 2605.27323, 2026) quantifies the tradeoff; **RenderMan XPU**
  (2025) is the hybrid CPU+GPU production data point; adaptive sampling revisited by *Forget Superresolution, Sample Adaptively*
  (arXiv 2602.08642, 2026). Adaptive stopping to a per-pixel variance/noise threshold (Zwicker 2015 survey, OFF-8) is the
  convergence orchestrator's core loop.
- **Cerid:** offline = wavefront over the existing CKIR RT stages; the "mode flag" flips launch strategy + sampler + AOVs, zero
  shader rewrites (exactly row 122's design). Deterministic per-tile seeding + checkpoint/resume for thousands of spp.

### Light transport core (OFF-1/2/3, but see ReSTIR) — the convergence base
- **Seminal:** unidirectional PT + NEE/MIS (Veach) — we have this (RT-4). BDPT (Veach 1997, OFF-2), VCM/UPBP (Georgiev 2012,
  Křivánek 2014, OFF-3) for SDS caustics.
- **2024-2026 — the big shift:** **GRIS / ReSTIR PT** (Lin 2022, *Generalized Resampled Importance Sampling*) gives ReSTIR a
  rigorous foundation with **variance bounds and an offline convergence guarantee** — reservoir path reuse that is unbiased in
  the limit. **ReSTIR BDPT** (2025, TOG) folds bidirectional connections + caustics into the resampling framework. This is why
  Cerid's offline GI base should be *ReSTIR PT driven to convergence*, with ReSTIR-BDPT as the caustic-robust extension — not a
  from-scratch Veach BDPT.
- **Cerid:** we already ship ReSTIR DI/GI (RT-5/7). OFF-1 = add the GRIS convergence guarantee + wavefront drive. OFF-2/3 =
  ReSTIR-BDPT for caustics before (or instead of) classical VCM.

### Path guiding (OFF-4) — the production convergence multiplier
- **Seminal:** practical path guiding / online **SD-tree** (Müller 2017); zero-variance-based guiding.
- **Production reality (SIGGRAPH 2025 course, Herholz et al.):** guiding ships in **Cycles, V-Ray, Corona, Karma, Hyperion**.
  Two families: **general** (scene-wide transport, SD-trees / spatio-directional) and **effect-specific** (caustics, guided NEE
  from many lights, guided volumetrics).
- **2024-2026 — the Cerid-enablers:** *Path Guiding for Wavefront Path Tracing* (Derin/Akyüz 2024, arXiv 2405.06997) makes
  guiding **GPU/wavefront-friendly** — the adaptive tree that "doesn't suit the GPU" is replaced with a memory-efficient
  GPU-native structure; **Online Neural Path Guiding with Normalized Anisotropic Spherical Gaussians** (Huang 2024, arXiv
  2303.08064, NASG) is the neural online-learned distribution; **ReSTIR PG** (SIGGRAPH 2025) unifies guiding with spatiotemporal
  path resampling — i.e. guiding *through the ReSTIR machinery Cerid already has*. Product/RIS guiding for the NEE side.
- **Cerid:** promote OFF-4 to right after OFF-1. Given we already have ReSTIR, **ReSTIR PG is the natural fit** (guiding reuses
  our reservoirs); NASG (neural) rides our NRC/coopvec infra. This is the biggest real-world "driven to convergence" win.

### Robust estimators (OFF-5) — the pathological tail
- **Seminal:** PSSMLT (Kelemen 2002), MMLT (Hachisuka 2014) — mutation-based for extreme low-probability paths (tight visibility,
  caustic networks).
- **2024-2026:** MCMC + guiding hybrids (*Path Space Partitioning and Guided Image Sampling for MCMC*, arXiv 2501.06214, 2025).
- **Cerid:** genuinely the last-resort estimator — build after guiding + ReSTIR-BDPT cover 95% of scenes. Keep as a robustness
  option, not the default.

### Spectral (OFF-6) — physically-accurate colour (science/aerospace/film)
- **Seminal:** **hero-wavelength spectral sampling** (Wilkie 2014) — one hero λ per path drives all directional sampling, MIS
  across wavelengths; **reflectance→spectrum upsampling** (Jakob-Hanika 2019) lifts RGB textures to smooth spectra.
- **Production:** *Spectral imaging in production* (SIGGRAPH 2021 course) is the practitioner reference (dispersion, thin-film,
  spectral MIS, sensor response, OCIO).
- **Cerid:** spectral is a sampler + BSDF-eval change on the wavefront integrator; the Huang elliptical-fibre hair BCSDF (B18)
  and thin-film (B5 slab) feed it directly. Needed for aerospace/science accuracy and film dispersion. Its own deep dive.

### Volumetrics + subsurface (OFF-7) — reference milky fur / cloud / skin
- **Seminal:** null-scattering / spectral-decomposition tracking (Kutz 2017); **random-walk SSS** (Chiang 2016) — the production
  brute-force-is-correct SSS.
- **2024-2026:** **progressive null-tracking** (SIGGRAPH 2023) for unknown majorants; **Rendering Participating Media Using Path
  Graphs** (arXiv 2404.11894, 2024) reuses volume transport; **ReSTIR SSS** (2024, TOG 3675372) brings reservoir resampling to
  subsurface paths; PDE-in-media solvers (arXiv 2506.08237, 2025). NVIDIA hybrid volumetric-PT + diffusion SSS with ReSTIR is the
  interactive counterpart our offline reference certifies.
- **Cerid:** the *ground-truth* volumetric/SSS the real-time B15 clouds / B18 fur / SSS approximations are measured against.
  Random-walk SSS + null-scattering on the wavefront integrator; ReSTIR SSS bridges to our reservoirs.

### Sampling, denoising, film output (OFF-8/9)
- **Sampling:** low-discrepancy (Sobol + Owen scrambling), blue-noise error distribution, adaptive to per-pixel variance
  (Zwicker 2015 survey). Firefly/outlier rejection from OFF-1.
- **Denoising (finishing only, never in the reference path):** **OIDN 2** (Intel Open Image Denoise) — a CNN RT filter,
  cross-vendor (CPU/CUDA/HIP/SYCL/Metal), **guided by albedo + normal AOVs**; OptiX AI denoiser is the NVIDIA counterpart. We
  should build an **OIDN-class AOV-guided CNN denoiser as the OFF finishing filter** (rides our NRC/coopvec neural infra), applied
  after convergence, comparing against the un-denoised reference.
- **Film output (OFF-9):** deep/multi-channel EXR, **cryptomatte** (per-object/material coverage IDs), **light-path expressions
  (LPE)** to split AOVs by transport path (diffuse/glossy/SSS/volume, direct/indirect). These are non-negotiable for a
  Blender/Nuke-class pipeline and for the CAD/CAM/VFX domains.

## What we read

- [Generalized Resampled Importance Sampling: Foundations of ReSTIR (Lin et al. 2022, TOG)](https://research.nvidia.com/labs/rtr/publication/lin2022generalized) — GRIS theory; variance bounds + **offline convergence guarantee** for ReSTIR. The single most load-bearing modern result for our offline mode.
- [ReSTIR PG: Path Guiding with Spatiotemporally Resampled Paths (SIGGRAPH 2025)](https://www.researchgate.net/publication/398668790_ReSTIR_PG_Path_Guiding_with_Spatiotemporally_Resampled_Paths) — unifies path guiding with ReSTIR resampling; the natural guiding fit for Cerid since we already have reservoirs.
- [Path Guiding in Production and Recent Advancements (SIGGRAPH 2025 course, Herholz et al.)](https://sherholz.github.io/siggraph2025-path-guiding-course/) — production adoption (Cycles/V-Ray/Corona/Karma/Hyperion), general vs effect-specific guiding.
- [Path Guiding for Wavefront Path Tracing (Derin/Akyüz 2024)](https://arxiv.org/pdf/2405.06997) — GPU/wavefront-friendly, memory-efficient guiding; solves the "adaptive trees don't suit the GPU" blocker.
- [Online Neural Path Guiding with Normalized Anisotropic Spherical Gaussians (Huang et al. 2024, TOG)](https://arxiv.org/pdf/2303.08064) — neural online-learned guiding distribution (NASG); rides neural infra.
- [Megakernel vs Wavefront GPU Path Tracing (2026)](https://arxiv.org/pdf/2605.27323) + [PBR-Book 4e §15 Wavefront Rendering on GPUs](https://pbr-book.org/4ed/Wavefront_Rendering_on_GPUs) — the offline GPU architecture.
- [Forget Superresolution, Sample Adaptively (2026)](https://arxiv.org/pdf/2602.08642) — adaptive-sampling revisit for PT (OFF-1/8).
- [ReSTIR Subsurface Scattering (2024, TOG)](https://dl.acm.org/doi/10.1145/3675372) + [Rendering Participating Media Using Path Graphs (2024)](https://arxiv.org/pdf/2404.11894) — modern volumetric/SSS reuse (OFF-7).
- [Intel Open Image Denoise](https://www.openimagedenoise.org/) + [OIDN 2 release](https://www.cgchannel.com/2023/07/intel-releases-open-image-denoise-2/) — AOV-guided CNN denoiser, cross-vendor GPU (the OFF-8 finishing filter target).

## Alternatives considered

- **Classical Veach BDPT (OFF-2) as the offline base, before ReSTIR** — worse for Cerid: we already ship ReSTIR PT/GI, and GRIS
  gives it offline convergence, so the ReSTIR path reuses everything. BDPT-with-caustics is better reached via **ReSTIR BDPT
  (2025)** than a parallel classical BDPT integrator.
- **Guiding via a CPU-style SD-tree (Müller 2017 as-is)** — the adaptive tree is GPU-hostile; use the **wavefront-friendly**
  (Derin 2024) or **neural NASG** (Huang 2024) or **ReSTIR PG** (2025) formulations instead.
- **Denoiser in the reference path** — never. The offline mode's whole point is an *unbiased reference*; OIDN is a finishing
  filter applied after convergence, and we always keep the un-denoised reference to certify it.
- **A second, separate offline renderer** — explicitly rejected by row 122: offline is a *mode* on the same CKIR integrators, so
  every improvement (guiding, spectral, volumetrics) lands once and serves both real-time and offline.

## Pitfalls / gotchas

- **GRIS convergence is a *modification*, not free** — vanilla ReSTIR is biased; the offline mode must flip on the unbiased
  contribution weights / MIS that the GRIS paper specifies, or the "reference" is subtly wrong. This is the OFF-1 correctness gate.
- **Guiding needs a warmup budget** — the online-learned distribution costs the first N spp; the convergence orchestrator must
  account for the training pass (or use the neural NASG which amortizes it).
- **Spectral + RGB textures** — Jakob-Hanika upsampling is mandatory or textured albedo desaturates; and spectral MIS across the
  hero wavelength is easy to get subtly wrong (energy loss at dispersion boundaries).
- **Null-scattering majorants** — a too-loose majorant tanks volumetric perf; progressive null-tracking (2023) removes the
  hand-tuned bound but adds state. Heterogeneous media (clouds) is where this bites.
- **Everything stays oracle-certified** — offline kernels are *the* reference, so a wavefront kernel that isn't bit-exact-vs-CPU
  (within the tier's tolerance) undermines every "matched accuracy" claim downstream. Non-negotiable.

## Next research (the per-pillar deep dives that branch from here)

1. **OFF-1 deep dive** — wavefront convergence orchestrator + GRIS offline-ReSTIR unbiasing (the ADR-level design).
2. **OFF-4 deep dive** — path guiding: ReSTIR PG vs wavefront-SD-tree vs neural NASG, chosen for Cerid's reservoirs + NRC infra.
3. **OFF-6 deep dive** — spectral: hero-wavelength + Jakob-Hanika upsampling + our thin-film/hair BCSDF integration.
4. **OFF-7 deep dive** — reference volumetrics + random-walk SSS (+ ReSTIR SSS bridge) as the ground truth for B15/B18.
5. **OFF-8/9 deep dive** — adaptive sampling + firefly rejection; OIDN-class AOV CNN denoiser; deep-EXR/cryptomatte/LPE film output.
