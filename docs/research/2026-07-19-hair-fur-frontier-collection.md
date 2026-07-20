# The Hair/Fur Frontier Collection — B18 research dossier

> Studied 2026-07-19 (pymupdf page-image reading; see `reference_read_pdfs_with_pymupdf` memory).
> Papers live in `docs/research/papers/hair_fur/`. Companion notes: `2026-07-19-huang-microfacet-hair.md`
> (Huang 2022 in full). This dossier maps EVERY technique to its B18 slice. The B18 target is the union:
> nothing here is out of scope (user: "defer nothing, full frontier").

## The collection (9 papers, all studied)

| # | Paper | Venue | B18 slice |
|---|-------|-------|-----------|
| 1 | Chiang et al. — *Practical and Controllable Hair and Fur Model* | EG 2016 | **B18-a ✅ shipped** |
| 2 | Huang/Hullin/Hanika — *Microfacet-based Hair Scattering Model* | EGSR 2022 | **B18-b** (microfacet half) |
| 3 | Yan et al. — *Efficient and Practical Near/Far Field Fur* (fetched separately) | TOG 2017 | **B18-b ✅ medulla shipped** (analytic form) |
| 4 | Zinke/Yuksel/Weber/Keyser — *Dual Scattering Approximation* | SIGGRAPH 2008 | **B18-c** (classic tier) |
| 5 | Hu/Zhu/Lin/Zhang/Wang/Yan — *Fur & Hair Multiple Scattering via Volumetric Approximation* | TOG July 2026 | **B18-c** (frontier tier) |
| 6 | Yuksel/Keyser — *Deep Opacity Maps* | EG 2008 | **B18-c** (self-shadow) |
| 7 | Huang/Zhou/Lin/Zhu/Yan/Wu — *Real-time LOD Strand-based Rendering* (arXiv 2405.10565 + EGSR 2025 final) | EGSR 2025 | **B18-d/e** (aggregated BCSDF + LOD) |
| 8 | Lipp/Jarabo/Wimmer/Bode — *Deferred Software Rasterization for Hair* | arXiv 2607.04230, 2026 | **B18-d/e** (raster architecture) |
| 9 | Bhokare/Montalvo/Diaz/Yuksel — *Real-Time Hair Rendering with Hair Meshes* | SIGGRAPH 2024 | **B18-d** (groom representation) |
| 10 | Wu/Shi/Darke/Kim — *Curly-Cue: Geometric Methods for Highly Coiled Hair* | SIGGRAPH Asia 2024 | **B18-d** (coily-hair authoring) |

---

## B18-c — multiple scattering + self-shadow (three tiers, all captured)

### Tier 1 (classic, real-time): Dual Scattering [Zinke 2008]
Ψ(x,ωd,ωi) = Ψ^G·(1 + Ψ^L). All quantities derive from the single-fiber BCSDF f_s (= our B18-a/b model):
- **Global**: Ψ^G ≈ T_f·S_f. T_f = d_f·∏_{k=1}^{n} ā_f(θ_d^k) (Eq 5) — n = strand count on the shadow
  path (from DOM/depth); ā_f(θd) = (1/π)∫_{Ωf}∫ f_s cosθd (Eq 6, front-hemisphere average — precompute
  as a 1-D θd LUT). Spread S_f = (s̃_f/cosθd)·g(θd+θi, σ̄f²), s̃_f = 1/π forward else 0; σ̄f² = Σ_k β̄f²(θ_d^k)
  (Eq 7/8 — variances ADD along the path).
- **Local**: Ψ^L·f_s ≈ d_b·f_back; f_back = (2/cosθ)·Ā_b(θ)·S̄_b (Eq 10). Ā_b = Ā_1 + Ā_3 with the
  closed forms Ā_1 = ā_b·ā_f²/(1−ā_f²) (Eq 11), Ā_3 = ā_b³·ā_f²/(1−ā_f²)³ (Eq 13); ā_b = back-hemisphere
  average (Eq 12). S̄_b = (s̃_b/cosθ)·g(θo+θi−Δ̄_b, σ̄_b²) (Eq 15); Δ̄_b, σ̄_b have power-series analytic fits
  (paper §3.2.2, footnote 2). Density factors d_f = d_b = 0.7 (0.6–0.8 realistic).
- **Weaknesses** (why the frontier tier exists): "dry" look, breaks on curly hair (local-similarity
  assumption), only R/TT considered, infinite-strand assumption.

### Tier 2 (frontier, matches path tracing): Volumetric approximation [Hu 2026, TOG 45(4)]
Hybrid geometry+volume. Code: github.com/NJUCG/efficient-fur-hair-ms-vol-approx.
- **Precompute** (voxelize, 128³ default ≈ 12 MB: 2×int8 dir + f32 density): per-voxel avg fiber dir ω̄ and
  σ_t^⊥ = 2rN/(πd²); anisotropic extinction σ_t(ω) = σ_t^⊥·sinθ. Albedo(ωi) = ∫_Ω S(ωi,ωo)dωo
  (1-D LUT over θi); σ_s = Albedo·σ_t, σ_a = σ_t−σ_s; **phase function P = S/∫S — the normalized BCSDF**
  (NOT SGGX/microflake: those can't do transmission).
- **Render**: camera ray → FIRST hit explicit strand (direct light, any BCSDF, NEE) → sample BCSDF outgoing
  → all further transport in the medium (≤1 geometric interaction — "aligns with rasterization pipelines").
  Single volumetric scatter: delta tracking; light connect; ray-marched transmittance.
- **Multiple scattering = re-weighted single scattering** (Wrenninge-style octaves, spectral):
  L = Σ_{i=0}^{N−1} L_i;  L_i = σ_s·(γ·**Albedo_RGB**)^i·L_light·P'_i·exp(−a^i·∫σ_t) (Eq 2) — RGB albedo^i
  keeps the color shift (scalar → color loss); P'_i = lerp(P, P_iso, 1−c^i) (Eq 3) progressive angular
  decorrelation; sample ONE octave stochastically. Baseline params (Ceres ℓ1-fit once per groom class):
  a=0.5, c=0.5, γ=1, N=3.
- **Results**: ≈ path tracing at 8.5× less cost (hair) / 3.8× vs Extended DS (fur); robust on curly where DS
  fails. Limits: over-smoothing of thin/sparse strands, concentrated backscatter highlights (white Afro).
- **CKIR note**: voxelize = compute kernel; delta tracking = our RT/compute; Albedo LUT = 1-D kernel; the
  phase IS our B18-a/b BCSDF. Fits the slice-c contract directly.

### Tier 3 (self-shadow): Deep Opacity Maps [Yuksel 2008]
3 passes: (1) light-view depth map → per-pixel z0 where hair starts; (2) opacity pass — K layers spanning
[z0+d_{k−1}, z0+d_k) per pixel (layers CONFORM to the hair shape ⇒ no striping), fragment adds its opacity
to its layer AND all behind, additive blend; depth+3 layers fit ONE RGBA target (n MRTs ⇒ 4n−1 layers;
3 usually suffice); (3) shading interpolates transmittance between layers. Beyond-last-layer points map to
the last layer. 8-bit depth suffices. This supplies dual scattering's n and T_f, and the EGSR25 pipeline's
shadow pass. ⚠ With strand-culling LOD, correct the stale depth with Lipp's Δ = −log(ρ_remain/ρ_total).

## B18-d/e — real-time strand systems (architecture menu, all compatible)

### Aggregated BCSDF + LOD [EGSR 2025] — thick-strand far field
- Aggregate a cluster of strands into ONE thick elliptical strand shaded by f^ours =
  S(φ,θd)·Σ_{p∈single∪{B̂}} M_p·N_p (Eq 7): inherited lobes M_p̂ = G(β̄_p̂^M,θh), N_p̂ = a_F^{n−1}·N_p with
  variance growth β̄_p^M = (n−1)σ̄_f²(θd)+β_p^M (Eq 8/9); extra B̂ back-hemisphere lobe A_B̂ = A_{1+}+A_1+A_3
  where A_{1+} = d_{1+}·a_B·Σ_{i=1}^n a_F^{2i−1} (Eq 10, d_{1+}=0.6) catches sparse-groom paths that classic
  DS misses; shadowing-masking S = P_s(φ)·a_F(θd)+P_n·𝟙 with P_s = (1−ρ)(1−cosφ), P_m = (1−ρ)c_m (c_m=0.2),
  P_n = 1−P_m−P_s (Eq 12–15). Strand count from unitless density ρ = n_total·r²/(|r_A||r_B|), n = ρl·√(…).
- LOD build: k-means guide hairs (D(i,j)=Σ_k|p^{i,k}−p^{j,k}|²) → 3-guide clustering → PCA ellipse fit per
  control-point set + 4 corner control points → triangular-prism linear skinning; levels ×4 cross-section
  area; 256 clusters, 5–6 levels, B-spline 16 CPs. Runtime: per-segment LOD, width via atomicMax, hysteresis
  switch L←L±1 vs screen threshold ε_w (Eq 16); 3 passes (shadow+DOM / shade / next-frame-LOD compute).
- 36 000× vs offline PT at FLIP ≈ 0.09; 10 ms GPU.

### Deferred software rasterization [Lipp 2026] — near/mid field, our B4-vis twin
- Pipeline: LOD prepass (1 thread/bundle → uint4 indirect args) → software raster (**1 WG/strand**,
  subgroup-size threads, register-only cooperative assembly — shared-mem layer data + subgroup shuffles of
  neighbor control points; two-pass tangents via finite differences; Hermite spline → subdivide → styling
  f(x) → polyline) → DDA 1-px lines → **64-bit atomicMin G-buffer**: 24b depth | 16b octa tangent | 18b uvw
  styling coords (re-query appearance at shade time!) | 6b AO → deferred per-pixel shade → filter → blend.
- TWO G-buffer layers: center-sample + conservative ⇒ single-pass strand-connectivity reconstruction, then
  tangent-oriented **elliptical bilateral filter** (r=5, σ∥/σ⊥ axes, σ_c=0.9, depth reject 1.45e−3) ⇒
  AA at 1 spp ≈ MSAA-8 reference at ~44 % of its cost (~3 ms total).
- LOD: stochastic strand removal N_LOD = clamp(⌈L·(N+δ)⌉,1,N), L = clamp(‖AABB‖/R_y·λ,0,1) (per-bundle
  screen AABB over layer quads; frustum cull); CP count √L-scaled, snapped to 2^k+1 (max 127). ⚠ Shadow/DOM
  maps go stale under culling → Beer-Lambert depth offset Δ = −log(ρ_remaining/ρ_total) (Eq 8).
- HW-raster pathology it avoids: 2×2 quad dispatch wastes ~75 % of lanes on 1-px strands.

### Hair meshes [Bhokare/Yuksel 2024] — the groom representation
- Groom = layered extrusion mesh over the scalp (root layer; bundle per root face); strand = spline through
  per-layer points at a fixed (u,v) barycentric; styling ops perturb; **nothing stored per strand** — strands
  are generated on the fly (huge bandwidth win vs linear hair skinning).
- **Hair mesh texture**: 3-D texture; root slice holds 2×2 texel blocks per quad face so BILINEAR HW filtering
  interpolates root positions; layer slices along r*; cubic Catmull-Rom → cubic Bézier → TWO quadratic Béziers
  (Truong 2020) with 3 intermediate slices ⇒ any curve point = 2 trilinear fetches + 1 software lerp — the
  texture-filtering unit does the spline math. Mesh-shader strand-level parallelism (or compute + software
  raster — exactly what Lipp does). 3 parallelism levels: bundle/strand/vertex.

### Curly-Cue [Wu/Kim 2024] — Type-4c/coily grooms (authoring-side)
- Strand c(t) = piecewise Catmull-Rom; frames {u,v,∂c/∂t}. **Fourier machinery**: displacement sequence
  x_i = p_{i+1}−p_i → per-component DFT; IDFT of the first 3 coefficients = the **low-frequency centerline**
  (+ closed-form shape-matching root translation t* = (1/(N+1))Σ(p_i−p*_i)); radial offsets r_i projected on
  {u,v} → DFT → amplitude/angle spectra 𝒜,𝒯 = centerline-agnostic high-frequency descriptors, transferred
  onto interpolated centerlines ⇒ wisps that COALESCE (phase locking) instead of linear-lerp destroying curl.
- Plus: physically-valid **switchbacks** (handedness reversals) via non-linear optimization (McMillen-Goriely
  static analysis), and **period skipping** within a curl. Regions: "spongy" near scalp, coalesced wisps
  further out — both must exist or coily hair reads as a wig.
- **CKIR/hesap note**: the DFTs are tiny 1-D FFTs — we own the FFT substrate; this is an authoring/cook-time
  kernel, feeding B18-d strand generation.

## Slice mapping (what each B18 sub-slice implements from this dossier)

- **B18-c**: DOM kernel (3-pass, conforming layers) + classic dual scattering (Zinke closed forms, ā_f/ā_b
  LUTs from OUR BCSDF) as the real-time tier, + the Hu 2026 volumetric MS (voxelize + albedo LUT + octave
  reweighting) as the gold tier that replaces "Extended DS" for fur. Gate: vs our own path-traced reference
  (the RT tier exists — B14/RT work), MRE-style comparison, both backends.
- **B18-d**: hair-mesh-style groom → on-the-fly strand generation (compute path; texture-filtering trick
  optional), aggregated-BCSDF thick strands for far field (EGSR25 Eq 7–15), Curly-Cue Fourier authoring for
  coily grooms; strand G-buffer per Lipp's 64-bit compressed layout (we HAVE the atomicMin visbuffer + DAIS).
- **B18-e**: composited frame = Lipp dual-layer reconnect + elliptical bilateral AA (plus our OIT tiers where
  transparency is needed); DOM shadows; LOD depth-correction Δ = −log ρ.
- **B18-f**: the LSS RT strand tier traces the SAME grooms; the path-traced reference validates B18-c tiers.

## Missing pieces (gaps in the collection — fetch when found)

1. ~~Zinke 2008 dual scattering~~ — **FETCHED** (`zinke2008_dual_scattering.pdf`), studied.
2. ~~Yuksel 2008 deep opacity maps~~ — **FETCHED** (`yuksel2008_deep_opacity_maps.pdf`), studied.
3. **Tafuri 2019 — Frostbite strand hair** (SIGGRAPH course): production playbook (analytic single-scatter
   integration, transparency strategy). Slides are on EA's site; not yet fetched.
4. **Yan 2015/2017b** (double-cylinder original + Extended DS / fur BSSRDF): context for what Hu 2026
   replaces; Yan 2017a already read (text) for the medulla.
5. **Wave optics**: Xia 2020 (wave-optics fiber scattering), Benamira 2021 (scattering+diffraction
   elliptical) — the beyond-geometric-optics tier for offline close-ups (OFF cluster, not B18).
6. **d'Eon 2011** (energy-conserving Mp) — foundation, already faithfully implemented via pbrt in B18-a.
7. **Marschner 2003** — the root model; superseded for implementation purposes by Chiang/pbrt (B18-a).
8. **TressFX / Epic groom docs** — engine-integration reference, not algorithmic.

## Cross-cutting implementation notes (CKIR)

- Every scattering quantity above derives from the single-fiber BCSDF we already ship (B18-a Chiang + B18-b
  medulla + Huang R-lobe next) — ā_f/ā_b/Albedo LUTs are 1-D precompute kernels over our BCSDF.
- The strand systems' primitives are ALL already in CKIR: atomicMin u64 G-buffer (B4-vis), DAIS deferred
  shade, subgroup shuffles (B-cmp), FFT (v10/B16), OIT (B17), RT reference (B14/RT detour).
- Bit-exactness: geometry/LUT/octave kernels are deterministic; the stochastic pieces (delta tracking,
  octave selection) use our triple32 streams (same discipline as ReSTIR/path tracer).
