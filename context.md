# Cerid — Live Context

> Short-term memory: "where are we now?" The master plan lives in `docs/ROADMAP.md`; the doc map in `docs/README.md`.
> This is a **DASHBOARD, not a changelog.** Each milestone's detail lives in its session log (`docs/sessions/YYYY-MM-DD-*.md`); this file summarises the *current* state and points there. Keep it lean (≤ 300 lines) — prune stale snapshots, don't stack them.

---

## Current focus — Phase 3.1.6 **v17 GPU compute (CKIR)** — on the **GPU-program-system detour** (D-007+D-008 MERGED)

> **▶▶ ACTIVE (2026-07-19, cont.): RAY-TRACING DETOUR — C3 `IRayTracingContext` + B9 CKIR RT, built TOGETHER (user-directed,
> a deliberate re-sequence ahead of B18/B19).** Frontier + engine research DONE (see the RT research; the C3↔B9 pairing + full
> scaffolding — 14-stage KStage incl. RT, RT builtins, per-stage create_program dispatch — already exist per D-007 rows 28/29).
> Gold scope = the planned C3/B9 rows + 2026 frontier (inline rayQuery · RT pipeline+SBT · SER · OMM · cluster-AS/mega-geometry ·
> LSS hair · RT-native ReSTIR/path-tracing). Decomposition: **[✅ RT-1 inline rayQuery core — DONE, GPU-verified]** → RT-2 full
> RT pipeline (raygen/hit/miss+SBT) → RT-3 frontier accel (OMM/SER/cluster/LSS) → RT-4 RT-GI (ReSTIR PT + path-traced reference).
> **[✅ RT-1 COMPLETE 2026-07-19]:** CKIR IR (`AccelStructDecl`/`RayHitResult`/`TraceRayClosest` + `trace_ray_closest` builder +
> a per-KStmt ext-operand list) · CPU oracle (brute-force watertight ray-triangle, the reference) · GLSL `GL_EXT_ray_query` +
> HLSL inline `RayQuery<>` emit (both COMPILE — the fix was `#version 460` + `cs_6_5`, no shaderc swap) · Vulkan RT DEVICE
> (`VK_KHR_acceleration_structure`+`ray_query`+`deferred_host_operations`+BDA enabled; `ray_query()` accessor) · a proper
> `VulkanRayTracingContext` (device-address buffers, BLAS+TLAS build, AS-descriptor-bound compute dispatch — fixed a realloc-UAF,
> an AS-handle leak, the core-vs-KHR proc name). **GPU rays == CPU reference EXACTLY** (t=[2,miss,miss,1]); `[rt]` 21/2, no
> regression ([oit] 42/6, [glsl] 26/5). **[✅ RT-2 — RT HARD SHADOWS 2026-07-19]:** generalized `trace_dispatch` (AS@0 +
> arbitrary storage bindings, upload/readback — the reusable inline-RT dispatch) + `engine/kir/.../ckir_rt.hpp` (the CKIR RT
> kernel library: `build_ray_trace_kernel` + `build_rt_shadow_kernel`). Per-point shadow ray P+t·(L−P) → visibility; GPU ==
> CPU ray-triangle oracle EXACTLY (0/64 mismatches, 56 lit / 8 shadowed under a floating occluder) — the first real RT effect
> and the direct B14 ReSTIR/GI visibility leaf. **[✅ RTAO 2026-07-19]:** `build_rtao_kernel` (ckir_rt.hpp) — per point, a
> runtime `For` over `samples` cosine-weighted hemisphere rays (Duff branchless tangent frame + triple32-hash sampling +
> `trace_ray_closest` per iteration + occlusion RMW): the **batch-of-rays-per-pixel LOOP pattern the path tracer needs**. GPU
> AO == CPU oracle **EXACTLY** (worst |Δ|=0, deterministic sampling; darkest 0.53 under the quad → ~1.0 open — the physically-
> correct ~50% for a 45°-subtended occluder). **[✅ RT REFLECTIONS 2026-07-19]:** NEW rich-hit IR `trace_ray_hit` (returns
> t + PRIMITIVE INDEX — `KStmtKind::TraceRayHit`, ext[8]=primId RayHitResult U32; wired GLSL `rayQueryGetIntersectionPrimitiveIndexEXT`
> + HLSL `CommittedPrimitiveIndex` + oracle best_tri). `build_rt_reflection_kernel`: reflect view about N → `trace_ray_hit` →
> SHADE the hit (fetch its flat normal by primId, Lambert) or sample a sky gradient on miss — the reflected-ray→hit→fetch→shade
> chain a path-tracing bounce needs. GPU colour == CPU oracle to 1 f32 ULP (worst 5.96e-8; 16 ceiling-reflecting / 48 sky).
> **[✅ RT-3 PATH-TRACING MEGAKERNEL 2026-07-19]:** `build_pathtrace_kernel` (ckir_rt.hpp) — the full light-transport integrator:
> runtime sample `For` loop (radiance RMW into out) with the diffuse bounce chain UNROLLED in C++ (CKIR `For` carries no registers,
> so origin/dir/throughput/radiance thread as SSA node ids). Per bounce: `trace_ray_hit` → miss=CmpEq(prim,0xFFFFFFFF) →
> radiance += select(miss, throughput·sky, 0) → fetch hit flat-normal + point → throughput = select(miss, 0, throughput·albedo)
> → resample cosine-weighted hemisphere (Duff-2017 branchless tangent frame; cosθ/π cancels pdf ⇒ ×=albedo). Deterministic
> triple32 sampling ⇒ GPU==CPU path-tracer oracle to transcendental ULP (worst 1.19e-7, mean 1.86e-8), with a low half-covering
> ceiling driving real multi-bounce GI spatial variation (radiance lum range 0.62→0.73 @ albedo 0.4). **[✅ RT-4 NEE + MIS
> AREA-LIGHT PATH TRACER 2026-07-19]:** `build_pathtrace_nee_kernel` (ckir_rt.hpp) — direct lighting done RIGHT: NEXT-EVENT
> ESTIMATION (sample the area light + shadow-ray it at EVERY path vertex) combined with the BSDF-sampled hit via MULTIPLE
> IMPORTANCE SAMPLING (Veach power heuristic β=2). Rectangular area light sampled analytically AND present in the AS (last tris)
> so BSDF rays hit it; separate triple32 NEE/BSDF streams. Emits 3 strategies (`PtStrategy` Mis/Nee/Bsdf) so the classic Veach
> UNBIASEDNESS check is testable — CPU: mean radiance MIS 0.6379 == NEE 0.6276 (Δ1.6%) == BSDF 0.6396 (Δ0.3%), all three converge
> ⇒ MIS weights correct. GPU: MIS==oracle worst 0.043 (grazing shadow-ray flips at the penumbra edge — the honest non-bit-exact
> RT contract), mean 0.0015, REAL soft shadow from an occluder. Both-backend emit+compile gate GREEN (GLSL + HLSL) ⇒ DX12/DXR
> mirror unblocked. ⚠ SCAR: CKIR `select` can lower to arithmetic (mix) ⇒ masking does NOT guard NaN — clamp `hit.t` finite (1e5)
> for miss point-reconstruction + guard every division denom with max(·,ε). ⚠ CPU oracle ~1ms/sample-iter (interpreted) ⇒ do
> convergence validation on GPU / keep CPU spp low + lean on low-variance MIS≈NEE for weight correctness. **[✅ RT-5 ReSTIR DI
> RIS RESERVOIR 2026-07-19]:** `build_restir_di_kernel` (ckir_rt.hpp) — RESAMPLED IMPORTANCE SAMPLING with a per-pixel reservoir
> (Bitterli 2020): stream M candidate light samples into a WEIGHTED-RESERVOIR-SAMPLING reservoir (target p̂=f·Le·G), keep the one
> survivor, pay ONE visibility ray (visibility reuse), estimate L=f·Le·G(y)·V·W with W=Σwᵢ/(M·p̂(y)) — provably unbiased. The
> reservoir is threaded as SSA through the UNROLLED candidate loop (WRS replace = mask-blend, all-finite); `frames` averaged in an
> outer runtime `For`. CPU: ReSTIR-DI 0.5881 == pure-NEE-direct 0.5877 (0.07% — unbiased). GPU: ==oracle ULP-EXACT (worst
> 1.19e-7, deterministic WRS ⇒ NO grazing flips) + real soft shadow. Both-backend emit gate GREEN. This is the spatial-less core
> (RIS+WRS+visibility reuse). **[✅ RT-5b/c ReSTIR SPATIOTEMPORAL 2026-07-19]:** the full Bitterli 2020 pipeline — 3 passes
> (`build_restir_temporal_kernel` → `build_restir_spatial_kernel` → `build_restir_shade_kernel`, ckir_rt.hpp) over a PERSISTENT
> 6-float/pixel reservoir [Qxyz,W,M], ping-ponged across frames on the GPU (in-out `trace_dispatch` bindings host-orchestrate the
> reservoir). TEMPORAL: merge the prev-frame reservoir at the same pixel (M-capped) — unbiased (0.1%) + **5.8× variance reduction**
> (warm rms 0.030 vs single-pass 0.175 on a visible light). SPATIAL: merge k neighbours (trig disk gather, u32 Mod/Div pixel
> coords) re-weighting each under this pixel's p̂, with the UNBIASED normalisation W=Σwᵢ/(Z·p̂(y)), Z=ΣMᵢ over domains actually
> containing y (2nd neighbour pass recomputes validity) — unbiased spatiotemporal to 0.08% + soft shadow, beats single-pass across
> the shadow edge (floored by the hard-penumbra 1-spp visibility variance the denoiser resolves). ⚠ SCAR: feeding the POST-spatial
> reservoir back as temporal history DARKENS 12% (compounding bias only GRIS/pairwise-MIS fix) → feed back the PRE-spatial reservoir;
> per-pixel rms conflates variance+bias, test bias via SPATIAL MEAN. Both-backend emit gate GREEN (Mod/Div/Cast wired in HLSL).
> `[rt]` now **375/13** ([glsl] 56/8, kir [rt] 9/3), no regression ([oit] 42/6). **[✅ C3b DX12 DXR MIRROR 2026-07-19]:**
> `Dx12RayTracingContext` (dx12_ray_tracing_context.hpp/.cpp) — the DX12 twin of the Vulkan RT context: ID3D12Device5 DXR
> BLAS/TLAS build (tier-1.1 gate) + inline-RayQuery compute dispatch. Root sig = ROOT SRV at t0 (the TLAS, bound by GPU VA) +
> a UAV descriptor table u1..u{maxBinding} (slot binding-1 ↔ register u{binding}) — matches the HLSL the emitter produces
> (`RaytracingAccelerationStructure as0:register(t0)` + `RWByteAddressBuffer bufN:register(uN)`). DEFAULT/UPLOAD/READBACK staging
> + explicit barriers (mirrors the DX12 compute context). Bumped `compile_hlsl_to_dxil` compute → **cs_6_5** (inline RayQuery;
> superset — full DX12 suite still 765/87). SAME CKIR kernels → HLSL → DXIL: RT-1 core EXACT, path tracer ULP-exact (1.19e-7),
> NEE/MIS worst **0.0433693 — byte-identical to Vulkan** (deterministic sampling ⇒ same grazing-flip points) ⇒ **VK≈DX12
> ESTABLISHED** (both agree with the shared CPU oracle within RT-traversal geometric tolerance). **[✅ RT-6 MULTI-INSTANCE TLAS
> 2026-07-19]:** `build_scene_instanced` on BOTH backends — one BLAS, N instances with per-instance row-major 3×4 world transforms
> (+ instanceCustomIndex/InstanceID); the hardware applies each transform in traversal. GPU == CPU oracle (world-space copies):
> t=[2,2,2,∞] identical on Vulkan + DX12 — the PORTABLE both-backend SCALE capability. NOTE: the literal SER (NVIDIA reorder) /
> OMM (opacity micromap) / cluster (RTX mega-geometry) are VENDOR-LOCKED hardware extensions (perf/niche, patchy cross-vendor
> support), out of the portable both-backend contract — deferred as HW-specific follow-ons. **[✅ RT-7 MANY-LIGHTS NEE
> 2026-07-19]:** `build_manylight_nee_kernel` (ckir_rt.hpp) — the integrator-breadth capability RIS/ReSTIR exist for: the N area
> lights live in a RUNTIME BUFFER (15f each: p0/eu/ev/nl/Le), each sample picks a light UNIFORMLY (⌊u·N⌋, Floor+Cast) and
> shadow-rays it, with pdf=(1/N)(1/areaₗ) ⇒ ×N·areaₗ weight + in-kernel |eu×ev| area. UNBIASED: GPU N-light kernel mean ==
> Σ per-light direct (0.16%); CPU eval smoke + both-backend emit gate (Floor/Cast wired in HLSL). The substrate a light-BVH /
> power sampling / multi-light ReSTIR resamples over. `[rt]` now VK **407/15** ([glsl] 62/9, kir [rt] 12/4) + DX12 31/4.
> **ALL FOUR requested RT frontier directions COMPLETE** (ReSTIR spatiotemporal · DX12 mirror · multi-instance TLAS · many-lights).
> **[✅ INTEGRATOR BREADTH 2026-07-19 — user directive: NO follow-ons, implement EVERYTHING incl. vendor extensions]:**
> **IB-1** `build_pathtrace_full_kernel` = full production path tracer (many-lights NEE+MIS + EMISSIVE hits primId→light + RUSSIAN
> ROULETTE unbiased ÷p + GI); GPU==oracle ULP, RR-on == RR-off exactly. ⛔ SCAR: `cf(node_id)` builds a constant = the node-id
> number (~80× bug), GPU==oracle BLIND to it → sanity-check magnitudes. **IB-2** ReSTIR GI (Ouyang 2021): 12-float reservoir
> [xs,ns,Lo,W,M], temporal + SPATIAL with JACOBIAN reconnection J=(cosθ_s^new·d_old²)/(cosθ_s^old·d_new²); unbiased (temporal 0.7%,
> spatiotemporal 1.1%) + variance win. **IB-3** light POWER sampling — CDF over luminance(Le)·area; unbiased + lower variance. All
> both-backend emit-gated. `[rt]` VK **1185/21** ([glsl] 86/11). **⭐ Adapter = RTX 4070 Ti SUPER — reports OMM / SER
> (VK_EXT/NV_ray_tracing_invocation_reorder) / VK_KHR_ray_tracing_pipeline / VK_NV_cluster_acceleration_structure → all
> RUN-verifiable here.** **[✅ FA-1 OPACITY MICROMAPS 2026-07-19 — RUN on the RTX 4070]:** all 4 vendor RT extensions ENABLED in
> vulkan_context.cpp (OMM · RT-pipeline · SER invocation-reorder · cluster-AS — detect + feature-probe + chain + devexts[28] +
> accessors). `build_scene_omm` (vulkan_ray_tracing_context.cpp): builds a VkMicromapEXT (2-state, per-tri VkMicromapTriangleEXT +
> usage counts + VK_BUFFER_USAGE_MICROMAP_*), attaches it to the BLAS triangle geometry (VkAccelerationStructureTrianglesOpacityMicromapEXT
> in pNext, per-tri OMM index buffer: tri0→0, rest→FULLY_OPAQUE), geometry NOT opaque. ⛔ the inline-rayQuery emit forced
> `gl_RayFlagsOpaqueEXT` which BYPASSES the OMM → changed to `gl_RayFlagsNoneEXT` (opaque scenes still auto-commit — [rt] 1185/21
> unchanged). RESULT: half-transparent OMM front triangle over a back triangle → 16 rays hit opaque micro-tris (t≈1), 18 PASS
> THROUGH transparent micro-tris to the back (t≈2) — alpha resolved in traversal, no any-hit. **[✅ FA-2 RT PIPELINE + SER
> 2026-07-19 — RUN on the RTX 4070]:** shaderc RT stages wired (raygen/closesthit/miss/anyhit/intersection/callable →
> shaderc_*_shader); `trace_rays_pipeline` (vulkan_ray_tracing_context.cpp) — VkRayTracingPipelineKHR (3 stages → 3 groups:
> general raygen/miss + triangles-hit), a base-aligned 3-region shader binding table from vkGetRayTracingShaderGroupHandlesKHR,
> `vkCmdTraceRaysKHR(w,h,1)`. The raygen uses SHADER EXECUTION REORDERING (GL_NV_shader_invocation_reorder: hitObjectNV +
> reorderThreadNV + hitObjectExecuteShaderNV) when `invocation_reorder()`. Result == oracle (t=[2,∞,∞,1]) WITH SER active — the
> perf reorder doesn't change results. **[✅ PORTABLE VENDOR RT P1–P5 2026-07-19 — user architecture: "in CKIR + warn when
> unsupported"; 3-layer split + any-hit alpha fallback]:** the vendor features are made PORTABLE + capability-gated. **P1**
> `RtCapabilities` query (rt_capabilities.hpp: RtFeature InlineQuery/RtPipeline/ShaderReorder/OpacityMicromap/ClusterAS) on both RT
> contexts. **P2** the RT PIPELINE authored IN CKIR — new IR (RayPayloadDecl/PayloadLoad/PayloadStore/TraceRayPipeline/ReorderThread/
> IgnoreHitIf + HitBary builtin) + `build_rt_pipeline_{raygen,closesthit,miss,anyhit_alpha}` + `emit_rt_stage_{glsl,hlsl}` (rgen/rchit/
> rmiss/rahit → GLSL GL_EXT_ray_tracing AND DXR HLSL lib_6_3). GLSL runtime-verified (t=[2,∞,∞,1] with SER), HLSL dxc-compile-gated.
> **P3** SER is a portable REORDER HINT: emit honors it (hitObjectNV flow) when the target has SER, DROPS it otherwise (plain
> traceRay / no-op) — perf-only, correct everywhere. **P4** portable `build_scene_alpha`: HW OMM if supported, else a NON-OPAQUE
> scene + the CKIR ANY-HIT ALPHA shader (`if(u+v<cutoff) ignoreIntersectionEXT`) — correct alpha-tested geometry without HW OMM
> (verified: 28 hit / 36 pass-through), + fell_back flag. **P5** portable `build_scene_scalable`: clusters if supported, else
> transparent standard BLAS (identical hits). ⛔ SCARS: `cf(node)` trap avoided; the SER hint / RT-pipeline stages couple (SER
> only exists in the pipeline). `[rt]` VK **1260/28**, DX12 **784/89**, kir 12/4 — no regression. **★★ VENDOR RT FEATURES NOW LIVE
> IN CKIR + GRACEFULLY DEGRADE WITH A DIAGNOSTIC — the portability mission applied to the frontier.**
> **[▶ B18 HAIR/FUR — RESUMED 2026-07-19, research-grounded]:** 10-paper frontier study → `docs/research/2026-07-19-hair-fur-frontier-collection.md`
> (+ `2026-07-19-huang-microfacet-hair.md`); PDFs in `docs/research/papers/hair_fur/` (read via pymupdf page-images — poppler absent).
> **[✅ B18-a CHIANG BCSDF]:** `ckir_hair.hpp` — the full Chiang 2016 near-field R/TT/TRT/TRRT (Bessel-I0 Mp, trimmed-logistic Np,
> Fresnel+Beer Ap, melanin/artist-RGB σₐ; Eq 6/7/8/9 verified against the paper). CPU **white-furnace** energy gate (σₐ=0 ⇒ albedo≈1,
> achromatic, f≥0) + absorption monotonicity. **NEW `build_hair_bcsdf_kernel`** ⇒ GPU==oracle **to-ULP on BOTH backends** (VK maxabs
> 2.4e-7 / DX12 1.8e-6). ⛔ SCAR: the GLSL **and** HLSL COMPUTE emitters both lacked `Round`+`Radians` (elementwise path had them) ⇒
> `emit_compute_kernel_*` returned FALSE — wired Round(roundEven/round, ties-to-even = oracle nearbyint) + Radians + Degrees in both.
> **[✅ B18-b FUR MEDULLA]:** Yan 2017 double-cylinder — but its TT^s/TRT^s are PRECOMPUTED Monte-Carlo tables (600MB→150KB), an
> offline-film shape that cannot be a portable bit-exact shader ⇒ implemented an **analytic closed form** preserving Yan's exact
> structure (Eq 4 unscattered+scattered): medulla chord d=2κ·cosγm/cosθt, interaction S=1−exp(−σ_m·d) gated on |h|<κ, and an
> **ENERGY-CONSERVING SPLIT** of the through-medulla TT/TRT into (1−S) unscattered · S·α_m scattered · S(1−α_m) absorbed — never
> additive. Scattered lobe = broad normalized Mp × **wrapped-Cauchy** azimuthal phase (the exactly-normalizable circular analog of
> Henyey-Greenstein — no elliptic integrals). Host-guarded: κ=0 emits ZERO extra nodes ⇒ B18-a graph byte-identical. Gates: furnace
> (α_m=1 ⇒ albedo≈1 — redistribution only), absorption monotone, κ=0 ≡ hair to 1e-12; GPU==oracle VK 1.3e-6 / DX12 1.3e-6.
> **[✅ B18-b HUANG MICROFACET R LOBE]:** Huang 2022 (EGSR, CGF 41(4)) — the NON-separable microfacet BCSDF; implemented its headline
> **analytic GGX azimuthal integral** (Appendix A Eq 41–44, α=0) for S_R = R/(8cosθo cosθi)·∫D dφm over the visible arc
> [max(Δφ,0)−π/2, min(Δφ,0)+π/2], frame rotated to φo=0 (kills all angle-wrap hazards). ⚠ `atan(tan u/s)` emitted as
> **atan2(sin u, s·cos u)** — identical on (−π/2,π/2) but tan OVERFLOWS f32 near ±π/2. Gate: closed form == an INDEPENDENT 20k-sample
> host quadrature of the same NDF to **5.3e-10** (the quadrature's own error floor) across β=0.08/0.3/0.6; GPU==oracle **byte-identical
> VK==DX12** (both maxabs 2.831e-07). `HairModel{Chiang,HuangR}` selects per material. kir [hair] 338/4, VK 24/4, DX12 12/3.
> **[✅ B18-b HUANG TT/TRT — B18-b now CLOSED]:** the non-separable lobes (Eq 23/24 via Eq 31/32) as a combined **MC-Simpson**
> estimator: a PINNED 1-D composite-Simpson quadrature over φm1, each node carrying ONE stochastic internal path (VNDF-sample ωh1
> → refract to ωt; TRT also samples ωh2 → reflects to ωtr), with the FINAL micronormal *determined* by the exit direction
> (ωh2=normalize(−ωt+ωo/η), ωh3=normalize(ωtr+ωo/η)) — a connect-to-outgoing step, structurally NEE. Determinism via triple32 on
> (lane,node) ⇒ a stochastic estimator that is still bit-reproducible GPU-vs-oracle. Absorption uses Huang's CORRECTED 2cosγt
> chord (Marschner's 2+2cos2γt drops a √). ⛔⛔ SCAR: total albedo came out **1.10 — GENERATING energy**. Diagnosed by PHYSICS,
> not code-reading (the transcription was correct): per-lobe furnace isolated it to TRT (0.2545 vs ~0.07); the η→1 limit proved
> normalization exact (TT=0.9965) and the TRT path structure exact (TRT=9e-10); an energy BUDGET showed TRT exceeded the 0.154
> available to it. Root cause = the VNDF helper orients the micronormal to the VIEW, but Huang's `ωtr=2|ωt·ωh2|ωh2−ωt` assumes
> RAY-facing ⇒ reflected into the wrong hemisphere. One-line re-orient ⇒ **R 0.085 + TT 0.761 + TRT 0.049 = 0.895** (≤1, the
> remainder being un-modelled TRRT⁺). `HairModel{Chiang,HuangR,HuangFull}` + per-lobe toggles select per material.
> **⛔⛔ ENGINE BUG FOUND + FIXED (affects ALL large kernels):** `emit_compute_kernel_{glsl,hlsl}` **HUNG** — `decl()` recursed into
> every child while `temped[]` gated only the temp EMISSION, never the DESCENT, so a node with k parents had its whole subtree
> re-walked k times ⇒ **exponential** traversal on deep diamond DAGs (Huang's stacked normalize/cross/VNDF chains). Added a
> `declseen` visit memo to BOTH emitters — behaviour-identical (a re-visit emits nothing), now linear. Emitted source: 35,896
> chars, i.e. the blowup was ALL traversal. Diagnosis lesson: a size-printf+fflush after the emit call settled in ONE probe what
> two wrong hypotheses (spirv-opt, select-inlining) had not. **Gates:** kir [hair] **350/5**; GPU==oracle both backends
> (VK 4.116e-06 / DX12 4.120e-06 maxabs — agreeing to 3 s.f. on a stochastic VNDF+Simpson kernel); full emitter regression
> **VK 433/49 · DX12 161/24** — nothing broken.
> **[✅ B18-c MULTIPLE SCATTERING + SELF-SHADOW — ALL THREE TIERS, both backends]:** new `ckir_hair_scatter.hpp`. The structural
> insight the 10-paper study bought us: **every scattering tier is an INTEGRAL OF THE SINGLE-FIBRE BCSDF**, so ONE precomputed
> LUT over our own model feeds all of them and any fibre-model improvement propagates for free.
> **(0) SHARED MOMENT LUT** `build_hair_scatter_lut_kernel` — integrates our BCSDF over the outgoing sphere (h-averaged) into
> [ā_f, ā_b, β̄f², Δ̄_b, σ̄_b²] per θd; this is simultaneously Zinke Eq 6/8/12/15 AND Hu's Albedo = ā_f+ā_b. Gate: ā_f+ā_b ==
> an INDEPENDENTLY WRITTEN furnace integral to **1.3e-7** (the f32 floor — two separate integrations of the same quantity).
> ⛔ SCAR: forward/backward were INVERTED — our own `hair_np` centring (Φ(p)=2pγt−2γo+pπ) proves R peaks at φo≈φi (backward)
> and TT at φo≈φi±π (forward), so Zinke's "forward" is the FAR half-cone; the `ā_f > ā_b` assertion is what caught it.
> **(1) ZINKE 2008 DUAL SCATTERING** — Ψ^G = T_f·S_f with T_f = d_f·ā_f^n and σ̄f² = n·β̄f² (locally-similar cluster ⇒ the Eq-5
> product collapses to a pow), plus the closed-form Ā_1/Ā_3 backscatter series. Gated on monotone depth attenuation, lobe
> broadening, and the headline effect: light hair 0.045 vs dark 4.7e-14 at n=4.
> **(2) DEEP OPACITY MAPS (Yuksel 08)** — per-pixel z0 → CONFORMING layer boundaries, cumulative layer opacity, piecewise-linear
> lookup. Gates: T=1 exactly in front, monotone darkening, opacity conserved (1.60 == 16×0.1), and ⭐ CONFORMANCE — 64 pixels
> whose z0 spans 3.2 agree to <1e-6 at equal RELATIVE depth (the property fixed-plane opacity maps lose; proves we built DOM).
> **(3) HU 2026 VOLUMETRIC MS — the GOLD tier** (TOG 45(4), July 2026; ~8.5× cheaper than PT and fixes dual scattering's curly-
> hair failure): anisotropic σ_t(ω)=σ_t^⊥·sinθ, **phase function = our NORMALIZED BCSDF** (the paper is explicit that microflake/
> SGGX cannot be used — their particles are opaque, hair transmits), and the octave series L_i=σ_s(γ·Albedo)ⁱP'_i·exp(−aⁱτ),
> P'_i=lerp(P,P_iso,1−cⁱ). Gates: σ_t parallel≈0 / perpendicular==σ_t^⊥ exactly, **octave-0 ≡ single scattering to 1e-5** (proving
> the series EXTENDS the sampled estimate, not a fudge), monotone brightening, γ=0 collapse, light-hair MS gain > dark. Plus a
> deterministic voxelizer (locally-coherent hemisphere-flip direction averaging + in-grid padding mask).
> ⛔⛔ TWO MORE ENGINE SCARS FIXED: (a) CKIR **buffer loads are INLINE** (re-read at use) ⇒ a post-loop RMW normalisation re-read
> slot 3 AFTER storing to it, collapsing σ̄_b² to exactly 0 and silently killing the whole backscatter lobe — materialize raw
> loads before any store; (b) the block-scope trap GENERALISED — it is not only `If` bodies: **sibling `For` loops bite
> identically** (`fbase` first used in loop 1 ⇒ temp declared there ⇒ "undeclared identifier" in loop 2). Rule: materialize every
> index base at top level before the first loop.
> **Gates:** kir [hair]+[scatter] **547/10**; GPU==oracle BOTH backends with VK and DX12 agreeing to every printed digit —
> scatter_lut 2.384e-06 · volume_ms 1.192e-07 · dom_build **0.000e+00 (bit-identical)** · dom_lookup 2.980e-08.
> **[✅ B18-d CORE + ✅ B18-e COMPOSITING — 2026-07-20]:** `ckir_hair_geom.hpp` — strand generation (bilinear layer interp +
> Catmull-Rom + helical styling) and the 64-bit atomicMin G-buffer (24b depth | 16b octahedral tangent | 18b uvw | 6b AO,
> round-trip gated), plus the B18-e **Lipp 2026 tangent-oriented elliptical bilateral filter** `build_hair_filter_kernel`:
> w_PQ = exp(−d∥²/σ∥² − d⊥²/σ⊥²)·exp(−‖ΔC‖²/σ_c²), wide ALONG the strand (bridges 1-spp gaps) and narrow ACROSS it
> (neighbouring strands stay distinct), with a HARD depth cut so nothing blends over a silhouette.
> **Gates (tests/kir/test_ckir_hair_filter.cpp, 5 cases):** partition-of-unity at F32 precision incl. clipped borders ·
> anisotropy ratio EXACT to exp(k²(1/σ⊥²−1/σ∥²)) for k=1..3 and the ellipse rotates with the tangent · zero cross-silhouette
> leak with the colour term neutralised (the depth guard alone must hold) · gap-fill >0.25× a hit while green from a strand
> 2 px away at the SAME depth bleeds <0.05× · convexity (no overshoot ⇒ no ringing halos).
> **GPU:** DISPATCH-verified both backends, **VK == DX12 == oracle at 1.431e-06**, deliberately dispatched 10×9=90 px over
> 2 groups of 64 so **38 tail lanes** exercise the kernel's `tid < N` bounds guard.
> **Beauty frame** (`build/hair_beauty.png`): added true sub-pixel coverage — bilinear 2×2 splat + order-independent
> 1−∏(1−a) — 18.7% of covered pixels partial. ⚠ Two lessons: opacity alone CANNOT antialias (snapping to `int(px)` quantises
> strands onto the lattice; the staircase only died with the bilinear splat), and the bilateral colour term must run in
> TONEMAPPED space or σ_c=0.9 is swamped by HDR radiance and the filter silently no-ops in the highlights.
> ⚠ Harness note: the beauty frame evaluates the filter formula directly, NOT via `eval_cpu_kernel` — the tree-walking oracle
> is ~11 ms/pixel and 640×520×121 taps is ~1 h. The shipped CKIR kernel is the gated artefact (CPU gates + both GPU gates).
> **[✅ B18-e CLOSED — 2026-07-20]:** the filter, sub-pixel coverage and 2x supersampling are done and
> gated. The REAL multiple-scattering tier is NOT. Diagnosis chain, in order, because each step invalidated the previous
> round of tuning:
>   1. **Deferred hair keeps ONE strand per pixel while ~148 overlap** (measured: px/world 249 x 746 px strand length x
>      172,800 strands / 869k covered px). Hard-sampling that IS line art — the "hand-drawn pen strokes" complaint. Four
>      rounds of tuning strand count / radius / resolution / filter width / roughness were all downstream of it. Fixed with
>      2x2 supersampling (resolve in LINEAR light; averaging display-encoded values darkens every edge).
>   2. **The Lipp filter at sigma_par=4 is calibrated for 1 spp WITH GAPS.** At density it is a directional smear — the
>      "oil painting" look. Dropped to 1.35 / radius 3, which is all anti-aliasing needs.
>   3. **Multiple scattering was a WRAP-DIFFUSE PLACEHOLDER, never the B18-c tier.** This is the one the user sensed as
>      "translucency broken": the BCSDF is verified (furnace, 20k-quadrature, GPU==oracle 1e-6), but the term that makes
>      hair a translucent VOLUME was a fudge with a hand-tuned gain.
> **The real tier is now WIRED AND CORRECT** (3 per-channel moment LUTs + build_dual_scatter_kernel per shaded pixel) behind
> `sc.dual_scatter`, now ON. **THE BUG THAT COST A ROUND:** Psi^G carries a directional spread S_f (Gaussian in
> theta_d+theta_i, width n*beta_f^2); applying it as a scalar multiplier on the direct lobe zeroed everything off the
> specular cone ⇒ black frame. T_f is the scalar attenuation, S_f is the spread of the SCATTERED component only.
> Measured fibre counts: key n=0.29, **rim n=9.73** (the dominant light, sits behind the groom, collapses first), fill 0.39.
> **FIX (read from Zinke §4, not the kernel signature):** Psi^G = T_f·S_f, and S_f is a DISTRIBUTION over incoming
> directions — the paper states it becomes delta(w_d − w_i) at n=0 — so Psi^G must be INTEGRATED against f_s, never used
> as a scalar. Only T_f = d_f·a_f^n multiplies the direct lobe (computed host-side from the moment LUT with the kernel's
> own nearest-bin mapping); f_back is pointwise and correct to add. T_f REPLACES the DOM transmittance (using both
> double-counts the occlusion). **VERIFIED PHYSICALLY:** platinum went from DARKEST (mean 0.144) to BRIGHTEST at the
> LOWEST exposure (0.255 @ 0.72, vs black 0.246 @ 1.70) — light hair scattering more than dark is Zinke's central claim
> and it now falls out of the model rather than a hand-tuned gain.
> **Engine defects fixed this round:** `6144U << 20U` silently truncates to 2 GiB (the shift is evaluated in unsigned int
> before it ever reaches the usize parameter); `TlsfAllocator` had a DOCUMENTED 4 GB ceiling (kFlIndexMax = 32) with
> nothing enforcing it — a larger pool indexes past `m_free_lists` and corrupts memory, surfacing as a SIGSEGV nowhere
> near the construction site, and a failed backing allocation was dereferenced blind by `init_pool`. Both now assert, and
> `TlsfAllocator::max_pool_size()` makes the limit checkable rather than folklore.
>
> **[✅ B18-d CLOSED — 2026-07-20]:** `build_strand_lod_kernel` — Lipp §3.2 read from the paper: Eq 2 screen-AABB LOD
> selector, Eq 1 stochastic `N_LOD = clamp(⌈L·(N+δ)⌉,1,N)` with a PER-BUNDLE δ, Eq 4/5 √L-scaled control points snapped
> to 2^k+1, Eq 8 `Δ = −log(β)` correcting the stale shadow/DOM depth that culling leaves behind. 4 CPU gates + Vulkan and
> DX12 dispatch gates, **both 2.384e-07 with ZERO discrete mismatches** on N_LOD and the control-point count — those are
> integers, so exact equality is asserted, because an off-by-one changes how much geometry a backend actually draws.
> ⚠ One gate initially asserted `E[N_LOD] == L·N` and failed (7.39 vs 6.74). Checked analytically BEFORE touching the
> kernel: the dither enters as `ceil(L·(N+δ))`, so its amplitude is L, not 1 — Eq 1 is biased by construction and the
> paper never claims otherwise (δ "smooths LOD transitions" = STAGGERING). Predicted E = 7.385, measured 7.3906: the
> kernel was right and the gate was wrong. It now measures POPPING directly — sweep L across a quantisation boundary,
> dithered vs an undithered control; undithered the groom-wide mean jumps a full strand at once (that jump IS the pop).
> **⛔ ENGINE:** `KOp::Exp2` was missing from BOTH compute emitters while `Log2` was present ⇒ the kernel would not emit
> at all. THIRD occurrence of the compute-emitter-lag scar, and the 2026-07-15 note had claimed "parity, future kernels
> won't hit this wall". Prose cannot establish parity — so the op set is now ENUMERATED against the oracle, ~20 further
> missing ops wired into both emitters, and a **completeness gate** (`test_ckir_emitter_completeness.cpp`) builds a
> one-op kernel per arithmetic KOp so occurrence #4 fails the moment an op is added rather than inside a later slice.
> ⚠ TOOLING SCAR: build checks were being filtered with `grep -E "error C|Linking"`, which does NOT match
> `fatal error LNK1168` — link failures were being read as successes. Check the exit status, not a pattern. The link
> failed because a background regression was RUNNING the executable being relinked; never overlap the two.
>
> **[✅ B18-f RT STRAND TIER — 2026-07-20; session log `docs/sessions/2026-07-20-b18f-rt-strand-tier-lss.md`]:** hair as a
> RAY-TRACED primitive, both backends. New `ckir_lss.hpp` (analytic ray/round-cone `lss_intersect`, conservative
> per-segment AABBs, a host-unrolled reference sweep) + a new CKIR statement `TraceRayCurves` returning
> `RtCurveHit{t, u, prim}`, with the oracle and BOTH compute emitters wired. `build_scene_curves` on the Vulkan AND DX12
> RT contexts builds a procedural-AABB BLAS (non-opaque — an opaque flag lets traversal skip the intersection shader) +
> a single-instance TLAS. **`VK_NV_ray_tracing_linear_swept_spheres` is Blackwell-only and NOT on this Ada 4070 Ti SUPER
> (verified with a positive control); DXR has NO swept-sphere primitive at ANY tier** — so the analytic path is not a
> fallback, it IS the strand tier, which is exactly why the intersector lives in CKIR and not in a vendor extension.
> **`build_rt_hair_shade_kernel` closes the loop:** every hit is shaded with the SAME B18-a Chiang BCSDF the raster tier
> uses — the fibre frame rebuilt from the hit segment's tangent (hence `prim` in the hit record: t and u cannot give you
> a tangent), h taken as the signed perpendicular miss distance in radius units (exact by construction, where a
> normal-derived h degenerates at grazing angles). **Gates:** `[lss]` 8/8 (6740 assertions); Vulkan 256 rays / 36
> segments / 45 hits with **ZERO hit-miss disagreements** vs the oracle, maxabs 8.3e-05; DX12 same scene, same seed,
> same oracle, 7/7; and the shading gate checks the IR fibre frame against a **second plain-C++ frame construction
> sharing no code with it**, agreeing to <2e-5 across 8 azimuthal offsets.
> **⛔⛔ THE EXPENSIVE SCAR — the round cone had FOUR independent defects, all invisible for a CAPSULE (rr == 0)** and so
> invisible to the first four gates: `d2 = m0 + rr²` (should be `m0 − rr²`), k-coefficients scaled by `m0`, the axial
> span tested against `m0` instead of `d2`, and the ray direction **not normalised** (the Quilez form assumes a unit
> direction). Together: **118 of 132 reported hits were off-surface** against a dense ray-march ground truth built in
> Python — which is how it was found. Re-reading the code had already failed twice. Corrected in all four homes (IR,
> oracle, GLSL, HLSL); the fixed Python then reported 57 hits, zero off-surface, matching the GPU's 57 exactly.
> **⛔ `rayQueryGenerateIntersectionEXT` requires tHit inside the ray's CURRENT range**, which traversal narrows on every
> commit — seeding the candidate search from the ray's ORIGINAL tmax clobbers nearer hits (committed t of 7.7e-05 where
> the answer was 0.95). Seed from the committed t instead. **⛔⛔ ENGINE: `eval_cpu_kernel` is SCALAR and had NO case for
> `Vec3`/`VecComp`/`Swizzle`** — they fell through to `apply_ternary`/`apply_unary`, which do not implement them, and
> evaluated to garbage **with no diagnostic**. Cost an hour on the shading gate: h was provably right and the frame
> vectors were provably right, but the vec3 carrying them into the BCSDF collapsed its z to 0, so φ was 0 instead of
> π/2 and the answer came out SYMMETRIC in h — physically plausible, entirely wrong. The evaluator now ASSERTS; compute
> kernels write vector maths component-wise on scalars (the vec3 forms are the raster tier's, where emitters lower them
> natively). **⛔ Re-materialising an already-materialised node allocates a SECOND slot nothing writes** — `TraceRayCurves`
> materialises its own results, so an extra `stmt_materialize` made t read back 0 on every lane and defeated the miss mask.
>
> **[✅ B18-f PATH-TRACED HAIR RENDERER — 2026-07-20/21; session `docs/sessions/2026-07-20-b18f-path-traced-hair-swatch.md`,
> bench `docs/bench/2026-07-20-hair-rt-swatch-perf.md`]:** the RT strand tier is now a WORKING renderer.
> `ckir_hair_rt.hpp` — a 3-bounce path tracer with NEE per light, a coloured transmittance shadow march (a fibre is a
> FILTER, not an occluder), a uniform-sphere indirect bounce, a studio env + an analytic ground plane with real contact
> shadow. `hair_swatch.hpp` — locks on a jittered grid, each a coherent ringlet, hanging under gravity, smooth analytic
> centrelines + per-endpoint smooth tangents, melanin→σₐ. Real 68 µm fibres, five hair types. Continuous smooth strands,
> sheen travelling along the fibre, coloured GI in the interior — NO beading, NO phantom.
> **⛔⛔ THE BIG BUG — f32 CATASTROPHIC CANCELLATION in the round-cone solve.** At realistic 68 µm fibre size viewed from
> ~1 unit away, the quadratic recovers a term of order m0·ra² by subtracting quantities of order |ro−pa|² — eight orders
> of magnitude apart, so f32 loses the radius entirely and commits hits ~9 radii off the surface (measured: mean |h| =
> 0.92 where a cylinder must give 0.5). **Fix: re-origin the ray at the segment before solving** (tsh = dot(pa−ro, d)),
> all four homes. After: mean |h| = 0.4996, and BOTH hardware gates got two orders of magnitude MORE accurate (VK
> 8.3e-05 → 4.8e-07, DX12 2.6e-05 → 7.2e-07). ⭐⭐ **THE DEFECT ARRIVED WITH CORRECTNESS** — at the fat placeholder radius
> the term survived, so every earlier render "looked fine"; realistic fibres made a latent precision bug the common case.
> Found by INSTRUMENT-FIRST (a clean unblended |h| AOV → |roff|/rad = 8.9 named the intersector) after four confident
> wrong hypotheses. Other fixes: flat→smooth tangents (per-segment specular dashes), the missing cosθi, plane-over-hair
> (white phantom), binary→transmittance shadows, MC speckle. Details + the discipline in the session log.
> **⭐ PERFORMANCE (measured):** ~194 ms per full-frame sample (1400×1000, 3 bounces, 3-light 8-step shadows, optimised
> SPIR-V, 4070 Ti SUPER). This is an **OFFLINE/film renderer** (converged ~384 spp ≈ 75 s). **Real-time is reachable but
> not with this path** — the levers all exist in-engine: B14 ReSTIR/SVGF denoise (~50-100×), B18-c DOM shadows in place
> of the 8-step march (~5-10×), 1 bounce, reduced-res+upscale. The pure-raster hair tier (B18-a…e) is the shipping-games
> real-time path; this RT path is its FILM reference. **⬜ open:** clang-tidy the new files; a grazing-limit BCSDF gate
> (the furnace test integrates over h and misses a narrow bad band — `test_ckir_hair_grazing.cpp` is a probe, not yet an
> assertion); the renderer geometry lives in the TEST, a production groom system is future work.
> **[▶ B19 3D GAUSSIAN SPLATTING — RESEARCH DONE 2026-07-21; dossier `docs/research/2026-07-21-3dgs-frontier.md`]:**
> the next frontier row (radiance fields as a first-class primitive). ⭐ KEY FINDING: **B19 rides on FOUR prior
> investments** — the forward splat render is a SORT + OIT-COMPOSITE problem (our GPU radix/onesweep + B17 A-buffer),
> training differentiates the rasteriser (v15/v16 autodiff), and the ray-traced variant (3DGRT) wraps Gaussians in
> procedural-AABB proxies (the B18-f LSS pattern via C3/B9). Frontier mapped along every axis with a mandatory winner:
> AA = Mip-Splatting + StopThePop; geometry = 2DGS/GOF (the bridge to B1 materials); compression = Self-Organizing
> Gaussians 20-40× (payload = our own HDR codec); ray-traced/relight = 3DGRT + Relightable 3DGS; training =
> 3DGS-MCMC/Taming 3DGS. **Decomposition:** B19-a forward splat core → B19-b Mip AA + StopThePop → B19-c 2DGS surfels
> + mesh extract → B19-d compression → B19-e ray-traced + relightable → B19-f differentiable training + LoD.
> **[✅ B19-a DONE 2026-07-21]:** the CKIR forward splat rasteriser renders a Gaussian scene. `ckir_gsplat.hpp` —
> two kernels: `build_gsplat_project_kernel` (the EWA splat: 3D anisotropic Gaussian → view-space covariance via the
> combined view·quat rotation, 2D covariance Σ′ = J·Σ_c·Jᵀ, conic = Σ′⁻¹, radius = ⌈3√λmax⌉, screen mean, SH deg-0
> colour, near-cull) and `build_gsplat_render_kernel` (per-pixel front-to-back `over` composite over depth-sorted
> splats, with the radius-bound cull). **Gates:** projection pinned to CLOSED-FORM geometry (on-axis mean/depth/conic/
> radius + behind-camera cull) + render composites near-red-over-far-blue correctly = 22 assertions; and a **GPU
> showcase renders a 3200-Gaussian rainbow sphere on Vulkan** (`build/gsplat_sphere.bmp`, centre lum 0.399 vs clean
> corner 0.035) — same kernels lower to GLSL and run. Depth sort is HOST-side for now (B19-a2 wires the GPU radix
> sort — the "sort" half of 3DGS; the composite half is B17's A-buffer). ⛔ compute tier is SCALAR so the covariance
> maths is component-wise; the radius cull is essential or the infinite Gaussian tail washes the whole frame.
> **[✅ B19-a2 TILE-BINNED RENDER 2026-07-21]:** the perf structure — each pixel composites ONLY its screen tile's
> splats, not the whole scene. `build_gsplat_tiled_render_kernel` (per pixel → its 16×16 tile → composite the tile's
> fixed-capacity bucket, front-to-back). **⛔ the CKIR `For` bound must be UNIFORM**, but a tile's list length varies —
> so the bucket has a fixed cap and the loop runs `cap` times masked by `(i < count[tile])`. **Gate:** the tiled
> render == the brute-force B19-a render PIXELWISE (oracle 2e-6; **GPU bit-exact, worst diff 0.00e+00** over 1024
> tiles) — tiling changes visibility structure, not the result. Buckets built HOST-side for now (B19-a3 = GPU
> count+scan+scatter). **⛔ THE GPU SORT GAP:** the existing radix sort is KEY-ONLY 32-bit; carrying a
> (tile,depth)→gaussian payload needs a 64-bit/payload sort extension (a B-cmp enhancement) — that + the GPU binning
> is B19-a3.
> **[✅ B19-b MIP-SPLATTING 2026-07-21 — the frontier ALIAS-FREE axis, Yu et al. CVPR 2024]:** the naïve 3DGS adds a
> fixed 0.3 to the 2D covariance diagonal but does NOT rescale opacity, so a splat's total energy (∝ opacity·√detΣ′)
> CHANGES as it shrinks below a pixel = aliasing. Mip fixes it (in `build_gsplat_project_kernel`, `cfg.mip`): a **3D
> SMOOTHING FILTER** (add the depth-scaled pixel footprint to the 3D covariance diagonal — frequency cap) + an
> **energy-preserving 2D MIP FILTER** (add the pixel footprint to the 2D diagonal AND rescale opacity by
> √(detΣ′/detΣ′_mip) so total energy is preserved EXACTLY). **Gate — the gold-standard invariant, measured:** shrink
> an isotropic splat 20× → true energy should fall 400× (√det ∝ s²). **MIP ratio = 400.0 (exact); naïve = 182.4
> (floored); the sub-pixel splat is 2.2× too bright under naïve (0.495 vs 0.225).** 26 assertions. mip defaults OFF
> so B19-a behaviour is unchanged.
> **[✅ B19-a3 ON-DEVICE DEPTH SORT 2026-07-21 — the sort half of 3DGS now runs on the GPU]:** closed **THE GPU SORT
> GAP** above. Extended the CKIR radix sort (`ckir_sort.hpp`, `build_sort_scatter(..., bool carry_val)`) into a
> **KEY-VALUE (payload) sort**: the value rides the exact permutation the keys are scattered by (ballot/rank/offset
> computed from keys ONLY, so the sort stays bit-exact); `carry_val=false` emits the identical graph ⇒ every existing
> caller (incl. the Vulkan radix-sort gate, 33 assertions, still green) is untouched. New gsplat kernels wire the
> pipeline: `build_gsplat_depthkey_kernel` (per splat → 24-bit quantised depth key over [dmin,dmax] + index payload;
> invalid ⇒ key 0xFFFFFFFF sorts last) and `build_gsplat_gather_kernel` (`sorted[i] = proj[order[i]]`). **Full pipeline
> = project → depthkey → 4-pass KV radix sort → gather, with NO host depth-sort crutch.** **Gates:** (CPU oracle)
> distinct-depth splats sort bit-exactly vs a host stable sort; (**real Vulkan**, `test_vulkan_gsplat.cpp`, 43
> assertions) the same pipeline runs end-to-end on the GPU and the gathered projected buffer == the host sort
> splat-for-splat, order is a valid permutation (XOR), depths ascending. KV payload sort also gated standalone in
> `test_ckir_sort.cpp` (n=16384, 4-pass ping-pong of keys AND values).
> **[✅ B19-a4 FULL GPU TILE BINNING 2026-07-21 — the last host crutch removed; the real Kerbl block rasteriser]:** the
> tile bin now runs entirely on-device. Five new CKIR kernels in `ckir_gsplat.hpp`: `build_gsplat_tilecount_kernel`
> (per splat → covered-tile count, half-open clamped rect) → **reuse `ckir_scan.build_scan`** (exclusive prefix-sum →
> per-splat instance offset + total T) → `build_gsplat_scatter_instances_kernel` (GRID-DRIVEN, N·max_cover threads, no
> CKIR `For`; emits key=tileID, val=splat-index; the `If`-guarded write) → **reuse the KV radix sort** BY TILE (stable ⇒
> since the input is already depth-sorted, within-tile depth order is preserved with NO depth bits in the key) →
> `build_gsplat_tile_ranges_kernel` (boundary-detect per-tile [start,end); nested `If`) → `build_gsplat_block_render_kernel`.
> **The block render is the real 3DGS topology: ONE WORKGROUP PER TILE, one thread per pixel, looping the tile's
> [start,end) — a VARIABLE `For` bound that is workgroup-uniform (every thread shares the tile), the exact CKIR `For`
> constraint** — so there is NO fixed bucket cap (B19-a2's `cap` is gone). **Gates:** (CPU oracle) the whole
> tilecount→scan→scatter→sort→ranges→block pipeline == the brute-force all-splats-per-pixel render **pixelwise, worst
> 0.000e+00** + per-instance range-partition checks (241 assertions incl. the 8th gsplat case); (**real Vulkan**,
> `test_vulkan_gsplat.cpp` `[bin]`, 38 assertions) the full bin + block render dispatched on the GPU (project/depth-sort
> host-scaffold; a3 gates the on-device sort) == the GPU brute render **bit-exact (worst 0.000e+00)**, T=290 over 16 tiles.
> ⛔ scar: the ranges kernel's `If` bodies needed `stmt_materialize` on the shared u32 store-index but must NEVER
> materialize the Bool guards (a bool has no temp type) — only the real-backend SPIR-V emit caught it, the CPU oracle
> passed the same graph. **⬜ NEXT: B19-c** 2DGS surfels + mesh (the bridge to B1 materials) or **StopThePop** per-pixel
> resort (view consistency) or **cross-backend** (wire all of B19 to DX12/HLSL — the whole line is Vulkan-only so far).
> **[✅ B18-f FULLY CLOSED 2026-07-21]:** path-traced hair renderer + real-time levers + recipes (`docs/recipes/` —
> new folder + AGENTS.md standing rule: study something → write a recipe). Tidy-clean at the gate, all touched gates
> green. Perf board `docs/bench/2026-07-20-hair-rt-swatch-perf.md` (194 ms/sample offline; 29 ms/frame real-time).
> **[✅ FA-3 CLUSTER-AS 2026-07-19 — RUN on the RTX 4070]:** `build_scene_clusters`
> (VK_NV_cluster_acceleration_structure / RTX Mega-Geometry) — the triangle → a CLAS via a GPU-DRIVEN INDIRECT build
> (`vkCmdBuildClusterAccelerationStructureIndirectNV`, opType BUILD_TRIANGLE_CLUSTER, implicit destinations, per-cluster
> `VkClusterAccelerationStructureBuildTriangleClusterInfoNV` with 32-bit indices) → a cluster BLAS (opType BUILD_CLUSTERS_BOTTOM_LEVEL,
> the CLAS address GPU-chained via the pass-1 dst-addresses buffer as pass-2's clusterReferences, AS-build barrier between) →
> readback the BLAS address → a standard TLAS instance referencing it. Traverses like a normal BLAS ⇒ t=[2,∞,∞,1] == oracle. ⛔
> SCARS (from validation): cluster srcInfos/count need `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT`, dst-arrays/implicit-data
> need `ACCELERATION_STRUCTURE_STORAGE_BIT`; the AS-build barrier access mask must be ACCELERATION_STRUCTURE_{WRITE,READ} only; and
> the cluster geometry must be marked `GEOMETRY_OPAQUE_BIT_NV` or (under RayFlagsNone) the simple trace never commits → all-miss.
> **★★ ALL THREE VENDOR RT FRONTIER FEATURES RUN + VERIFIED on the RTX 4070 (OMM · RT-pipeline+SER · cluster-AS) — the complete
> gold-standard cutting-edge RT system is DONE, no follow-ons.** `[rt]` VK **1212/24**. Correctness = GPU-RT vs CPU
> `geometry-bvh` watertight reference within tolerance + VK≈DX12 (RT traversal is NOT bit-exact). Unblocks the B14 ReSTIR RT
> visibility leaf. **B18 hair PAUSED at B18-a (BCSDF core landed + energy-verified, feeds the RT path tracer):** `ckir_hair.hpp`
> = Chiang R/TT/TRT/TRRT (Bessel-I0 Mp, trimmed-logistic Np, Fresnel/Beer Ap, cuticle tilt, melanin/RGB→σₐ), **white-furnace
> albedo = 1.00009 (energy-conserving to 0.02%)**, shape-polymorphic (scalar→monochrome elementwise / vec3→colour raster),
> both-backend GLSL+HLSL emit+compile gate green, Math-Mandate clean. Remaining hair (B18-a data dispatch, B18-b fur+Huang, c/d/e)
> resumes on top of RT (LSS strands + path-traced dual-scattering reference).
>
> **▶▶ LAST SHIPPED (2026-07-19 — session log `docs/sessions/2026-07-19-b17-oit.md`; accuracy board `docs/bench/2026-07-19-oit-tier-scoreboard.md`):** **B4 CLOSED (user-committed); B17 OIT — ALL THREE tiers landed + device-verified BOTH backends.**
> **[✅ B17-b MBOIT]** (Münstermann 2018, hero glass/foliage tier): absorbance-weighted 4-power-moment generation + the
> Peters-Klein Hamburger reconstruction (`oit::msm_hamburger_scalar`, a scalar transcription of the moment-shadow `msm_hamburger`
> since the compute emitter is scalar-only) → per-fragment transmittance → composite, over the SHARED deferred store. Lowers
> to-ULP (both backends); 4 moments resolve 2 depth masses **EXACTLY** ⇒ bit-exact at 2-layer (glass), beating WBOIT (30 LSB).
> **▶ LIFTED to high depth complexity (6-POWER-MOMENT extension, `oit::msm_hamburger6_scalar`):** the Hamburger solve
> generalized to a 4×4 Hankel Cholesky + a CUBIC root-solve + a Gauss-Radau form factor → resolves **3 masses**, **~1 LSB from
> exact at 3-layer complexity where WBOIT is 18 LSB off — an 18× win**, both backends. Exact-capacity = ⌊moments/2⌋ masses.
> **MBOIT BEATS WBOIT at every depth complexity within its moment budget — no recorded loss.** No regression (VK 715 · DX12 735). Tidy-clean.
> **B17 ORDER-INDEPENDENT TRANSPARENCY** — all three tiers built + device-verified on **both backends** (one CKIR IR → GLSL+HLSL):
> **[✅ B17-a WBOIT]** (McGuire-Bavoil 2013 — the cheap single-pass tier): a NEW gpu-context capability — **render-to-texture float
> MRT + per-attachment blend equations** (Vulkan `vkCmdSetColorBlendEquationEXT` dynamic state; DX12 `IndependentBlendEnable` static
> PSOs) — accum (RGBA16F, additive) + revealage (R16F, multiplicative) accumulate in ONE order-independent pass, a full-screen
> composite resolves over the background (`draw_wboit`, both backends). CKIR shaders (`ckir_oit_test.hpp`) + f16-aware CPU oracle;
> observable RGBA8 matches to **≤2 LSB** (f16 accum + f32 divide ⇒ to-ULP tier) VK==DX12. **[✅ B17-c A-BUFFER exact reference]**
> (Carpenter 1984): two portable CKIR **compute** kernels (`ckir_oit.hpp`) — deferred per-fragment store (static slot = fragment id
> for the fixed-coverage reference scene) + a per-pixel depth **sort network** + the exact front-to-back `over` composite. Pure f32
> mul/add/sub on a deterministic order ⇒ **BIT-EXACT** vs `eval_cpu_kernel` and VK==DX12 (worst |Δ|=0). The GROUND TRUTH the
> approximate tiers score against. **[✅ QUALITY SCOREBOARD]** WBOIT 14 LSB off exact (4-layer) · MBOIT bit-exact at 2-layer where
> WBOIT is 30 LSB off — the accuracy board `docs/bench/2026-07-19-oit-tier-scoreboard.md`. B17 all three tiers CLOSED both
> backends (VK 715 · DX12 735; OIT VK 29 · DX12 26). Tidy-clean (win-tidy). **⚠ PRACTICAL CEILING MAPPED (both explored + removed):**
> **8-moment power = compile WALL** (quartic reconstruction too big — driver re-unrolls, MEASURED hang); **trig moments = quality
> WALL** (Fourier/Fejér compile fast but too blurry — 80 LSB, LOSE to WBOIT's 14); **per-pixel-hoisted runtime-loop = the
> architecture WORKS (diagnostic: trivial body runs instantly) but the 8-moment quartic body is too complex for the shader
> compiler EVEN EMITTED ONCE** — the wall is reconstruction complexity, not unrolling. **6-moment (3 masses) is the DEFINITIVE
> ceiling; the EXACT A-buffer (shipped, bit-exact) is the tool for arbitrary complexity. B17 DONE — WBOIT beaten across the
> board.** **[✅ B17-c SCALABLE — the three carry-forwards ALL SHIPPED (2026-07-19, both backends; perf board
> `docs/bench/2026-07-19-oit-tier-perf.md`):** (1) **atomic linked-list A-buffer** — NEW **value-returning atomics** in CKIR
> (`atomic_add_fetch`/`atomic_exchange`, `AtomicResult` impure leaf, all 5 emitters); race-built lists, sort-resolved, match the
> static exact reference **BIT-FOR-BIT** (worst |Δ|=0) VK==DX12; gold interleaved node-pool (≤8-binding cap). (2) **stochastic
> transparency** (Enderton 2010) — S deterministic-hash sub-samples, mean = UNBIASED `over` (converges ~1/√S; TAA=1/frame);
> triple32 hash GPU==oracle **bit-exact** both backends — REQUIRED making the **oracle's u32 arithmetic wrap mod-2^32**
> (`apply_binary_typed`; it did integer ops in f64 with no wrap → a portability hole; full kir suite green 34759). (3) **GPU perf
> board** (`[.oit-bench]`): static store-based tiers cheapest (~0.28 ms) & **MBOIT accuracy is FREE** (memory-bound); atomic list
> ~10× (unbounded-depth price); stochastic cheapest unbounded tier per-frame (~0.061 ms @ S=1). Tidy-clean 3 targets; OIT VK 42 ·
> DX12 35; full VK 84c · DX12 746a. **B17 = 5 tiers, both backends, accuracy + perf boards, NO loss.** Next: **B18 hair/fur.**
>
> **▶▶ (2026-07-18 — session log `docs/sessions/2026-07-18-b16-close-units-typing.md`):** **B16 CLOSED (DoD green).**
> The B16/B4 batch (displaced-geometry ocean + mesh-shader fast path + promotion, from 2026-07-15/16) passed its full close-out. The
> close-out full sweep **peeled a tidy onion** of pre-existing issues prior sessions missed by running test *binaries* instead of full
> ctest+shipping — all root-caused + fixed (SANITY, no debt): (1) tidy on 36 touched files (mesh-emitter locals, `ocean_grid` globals
> → `k`-prefix); (2) **12 non-ASCII TEST_CASE names** → ASCII ([[feedback_ascii_only_test_names]]); (3) `std::pow`×3 → `crd::math::pow`
> (Math Mandate); (4) a win-shipping **`C1001`/`C4789`** traced to the **`#deps 0` landmine** (win-shipping had the English
> `msvc_deps_prefix` + VS-bundled `CMAKE_COMMAND`, re-armed by a GLOB reconfigure → stale pre-`m_stmts` KGraph obj) — wiped +
> reconfigured with standalone CMake [[feedback_stale_toolset_path_in_build_dir_wipe_dont_sed]]; (5) the **untagged-physical guard**
> resolved by a **full gold-standard units typing** of the CKIR sim/GI configs (atmosphere/ocean/ddgi/svgf): every dimensional field
> → `Length`/`Velocity`/`Acceleration`/`Angle` + a custom `InverseLength` for 1/km extinction coeffs (SI-stored, `value_in` at the IR
> boundary, **all round-trips bit-exact**); dimensionless tuning knobs stay raw with honest markers on the two heuristic
> false-positives. **Verified green:** win-debug (full 5061), + focused build+test of the 7 touched targets on shipping/asan/release +
> tidy build; bit-exact HELD (atmos 1562 · kir 34725 · vk 33023 · dx12 30821). ⚠ transient MSVC LTCG `C1001` cleared on retry (known,
> BUILDING.md). B16-a-1 remaining scope (SSR/refraction/underwater/caustics) + B4 remaining carry forward.
>
> **▶ B4 PROGRESS (2026-07-18, post-B16):** the B4 sub-slices landed in order — **DX12 device mesh render** ✅ (IR→DXIL `ms_6_5`
> on-ramp, `OPTIONS7 MeshShaderTier` gate + `Device2`/`CmdList6`, hand-rolled mesh PSO stream, `create_mesh_program`+`draw_mesh`, +
> the bindless-depth ocean-meshlet variant) · **TASK / amplification** ✅ (both backends: `EmitMeshTasksEXT`/`DispatchMesh` + a
> task→mesh payload, `create_task_mesh_program`) · **B4-tess** ✅ (both backends: `TessControl`/`TessEval` GLSL **and** HLSL
> emitters + device render — Vulkan VS+TCS+TES+FS shader objects + EDS2 `PATCH_LIST`/patch-control-points; DX12 HS/DS graphics PSO +
> `4_CONTROL_POINT_PATCHLIST`; the `[tess]` quad subdivides 8×8 + domain-displaces ×1.3, proven per-vertex on both). Both full raster
> suites green (vk 642/58 · dx12 555/57).
>
> **▶ B4-vis STARTED (2026-07-18): NANITE virtualized geometry.** **[✅ B4-vis-1 COMPUTE SOFTWARE RASTERIZER]** the Karis
> software rasterizer (the micropoly path HW raster can't do well) runs bit-exact in CKIR: new `BufferAtomicMin` stmt (all 5
> compute-kernel emitters + oracle; MIN order-independent ⇒ bit-exact) + `Ceil`/`Clamp` completed in the imperative-kernel path
> of all 5 emitters + `ckir_visbuffer.hpp` `build_sw_raster_visbuffer` (one thread/triangle, edge-function coverage, barycentric
> depth, packed `(depth<<idBits)|triId` u32 via atomicMin — nearest wins; NO 64-bit atomics; backend-independent depth). The
> `[visbuffer]` test: 3-triangle scene, **bit-exact GPU==oracle on Vulkan (9) + DX12 (8)** + 5-backend emit gate (12). **[✅
> B4-vis-2 DEFERRED ATTRIBUTE SHADE (DAIS)]** `build_deferred_attr_shade` — one thread/pixel reads the vis key, fetches the 3
> verts, reconstructs PERSPECTIVE-CORRECT barycentrics (`Σ(e_i/w_i·a_i)/Σ(e_i/w_i)`, one divide) + interpolates once per visible
> pixel. Visibility bit-exact; the shade matches the oracle **to ~2 f32 ULP (1.18e-7, identical Vulkan+DX12 — bit-exact with each
> other; f32 divide is ~2.5 ULP on GPUs, not correctly-rounded, like the ocean spectrum)**; perspective proven (centroid ~8 not
> 14). No regression (DX12 [compute] 111 · Vulkan [kernel] 311).
>
> **[✅ B4-vis-3 HZB TWO-PASS OCCLUSION CULL]** `build_hzb_downsample` (max-depth mip pyramid, one dispatch/level in ONE buffer,
> barriered) + `build_cluster_cull` (per-cluster AABB-vs-HZB: `visible = near_depth <= max_hzb ? 1 : 0`, conservative). MAX +
> compare order-independent ⇒ **BIT-EXACT** both backends. `[hzb]` test: 16² near-wall/far split → pyramid → 3 clusters
> (behind=cull, open=visible, in-front=visible), GPU==oracle==analytic (VK 8 · DX12 7); 5-backend emit gate 12. GPU-driven
> culling = the Nanite scale lever. No regression (DX12 [visbuffer] 23 · Vulkan 27). **[✅ B4-vis-4 HW-RASTER VBUFFER]** the hybrid
> split (HW raster for big triangles): `KBuiltin::PrimitiveId` wired into GLSL/HLSL FS emitters + a native **R32_UINT** vis target
> on both devices (`create_visbuffer_target`/`draw_visbuffer`; DX12 `pso_for` gained an RT-format for the R32_UINT PSO). The
> `[raster][visbuffer]` test: fullscreen quad → FS writes `gl_PrimitiveID` → R32_UINT; every pixel is primitive 0/1, each ~half —
> Vulkan 10 · DX12 7. **✅✅ B4-vis COMPLETE** — software rasterizer (bit-exact) + DAIS (to-ULP) + HZB cull (bit-exact) + HW-raster
> VBuffer (both devices). No regression (DX12 [raster] 562 · Vulkan 652).
>
> **[✅ MULTI-FIELD PAYLOAD]** the task→mesh payload now carries up to 4 uint fields (`KBuiltin::TaskPayload{,1,2,3}`, fixed
> 4-field struct so task+mesh layouts match; `KEntry.task_payload[4]`+`n_task_payload`) — a meshlet passes bounds/LOD/material,
> not one uint. `[mesh][task]` 3-uint payload→RGB, both backends (vk 12 · dx12 8), single-field back-compat held. No regression
> (DX12 [raster] 572 · Vulkan 665).
>
> **[✅ GPU-DRIVEN COMPUTE-CULL → INDIRECT MESHLET DISPATCH — BOTH BACKENDS]** the Nanite scale loop, no CPU round-trip:
> `build_meshlet_cull` (compute pass atomicAdds the survivor count into an INDIRECT args buffer `{gx,1,1}`) → `draw_mesh_indirect`
> (`vkCmdDrawMeshTasksIndirectEXT` / DX12 `ExecuteIndirect`+DISPATCH_MESH command signature) — the mesh-workgroup count decided on
> the GPU. Plumbing: `compute_usage::indirect` (+ Vulkan CONCURRENT queue-sharing), `ComputeBuffer::native_handle()`. `[indirect]`
> test: 8 meshlets, 5 visible → cull writes 5 → exactly 5 render, 3 culled never dispatch — VK 14 · DX12 9, device-verified. No
> regression (VK [kernel] 329 · [raster] 679 · DX12 [compute] 126 · [raster] 581).
>
> **[✅ PER-PRIMITIVE VRS FROM THE MESH — BOTH BACKENDS]** the mesh emits a per-primitive shading rate (`shading_rate` →
> `gl_MeshPrimitivesEXT[].gl_PrimitiveShadingRateEXT` / `out primitives … SV_ShadingRate`); new `draw_mesh_vrs` (REPLACE/OVERRIDE
> combiner). `[mesh][vrs]`: 2×2 primitive rate → full coarsening (n_equal 512) on VK + DX12. **[⏭ WGSL `texture_2d_array`
> cascade — scoped to the WebGPU milestone]** WGSL is compute-only here (no raster emitter); the cascade port needs the WGSL
> raster path = the browser/WASM deployment goal, out of D-007 scope. **✅✅ B4 CLOSED** — every substantive line done (mesh/task/
> tess, ocean meshlets, full Nanite vis-buffer 1-4, multi-field payload, GPU-driven indirect dispatch, per-primitive VRS);
> only cluster-AS (→ B9/C3 RT) + the WebGPU WGSL cascade remain, both correctly out of scope.
>
> **▶▶ NEXT UP (D-007):** **B17 OIT → B18 hair → B19 3DGS → RT tier (C3/B9)** (on user command). (B16/B4 detail → the session
> logs + the D-007 master doc.)

> **▶ THE ONE MASTER DOC: `docs/detours/D-007-gpu-program-system.md`** — a single ordered subslice table (D-007 shader IR +
> D-008 device convergence, merged 2026-07-11). Foundation + raster DONE: Phase A ✅ · fan-out ✅ · B0 ✅ · B3-a/a′ ✅ ·
> C0/C1/C2 ✅ (device convergence CLOSED, I1+I2 shut, one VkDevice) · **B3-c/d ✅** (GLSL + HLSL VS+FS emitters) ·
> **C4 ✅** (DX12 `IRasterContext` + the real DX12 create-program seam + graphics-PSO draw) · **B3-e ✅** (ONE CKIR
> triangle draws on BOTH Vulkan + DX12 — the IR-authored end-to-end) · **B1-a ✅** (fragment derivatives) · **B1-b ✅**
> (`discard`/alpha-test) · **B1-c ✅** (interpolation qualifiers flat/noperspective/centroid/sample — all four OBSERVABLY
> tested on Vulkan + DX12, incl. MSAA render targets for centroid/sample) · **B1-d ✅** (frag-depth write + early-Z flag +
> conservative depth — NEW depth-buffer infra both backends; observable frag-depth split; early_fragment_tests + DepthGreater/
> Less compile-validate) · **B1-e ✅** (VARIABLE-RATE SHADING — ALL THREE rate sources: per-draw pipeline + per-primitive
> shader-output (`gl_PrimitiveShadingRateEXT`/`SV_ShadingRate`) + attachment image; observable 2×2 blockiness on Vulkan +
> DX12) · **B1-f ✅** (conservative raster overestimate + inner coverage + fragment interlock/ROV + FS storage buffers —
> observable covers-more/edge-split/deterministic-RMW on BOTH backends, IDENTICAL numbers; 3 Vulkan VUIDs + 1 DX12
> debug-layer error found-and-fixed). **▶ B1 COMPLETE — the fragment-stage frontier is done.** · **B2 ✅ core (a–d)**
> (TEXTURE & SAMPLER system — separable texture/sampler IR types; B2-a 2D sampling + upload/bind infra · B2-b full sample-op
> family Lod/Grad/Cmp-shadow/texelFetch/gather/textureSize · B2-c dims 1D/3D/Cube/2DArray/CubeArray + 2DMS-emitter · B2-d
> BINDLESS descriptor arrays w/ dynamic per-fragment index; ALL observable + IDENTICAL both backends; 2 CubeArray VUIDs
> found+fixed (`imageCubeArray`); 2DMS observable + B2-e sampler-feedback documented as DX12-only/render-to-texture
> boundaries). · **B5 ✅ (a–c)** (OpenPBR SURFACE MATERIALS — `ckir_material.hpp`: B5-a surface contract + deferred G-buffer
> (NEW MRT device capability, both backends) · B5-b the FULL OpenPBR 1.1 slab (~37 fields: specular/coat/fuzz/transmission/
> subsurface/thin-film/thin-walled) via an extended 8-attachment G-buffer · B5-c fresh `ShadingModel`{PBR+NPR toon/cel/gooch/
> outline/hatching} + `AlphaMode`{opaque/masked/translucent/additive} tags + masked-discard; material = surface, lighting-
> agnostic per ADR-0102 D3; every channel + tag reads back pixel-IDENTICAL both backends). · **B6 ◧** (MaterialX-parity NODE
> LIBRARY) — **B6-a ✅** the OPERATOR set (`ckir_nodes.hpp`, ~60 nodes: math/logical/conditional/channel/adjustment/
> compositing/convolution) bit-exact on the CPU oracle + observable both backends (overlay per-channel branch 57,36,14 /
> 244,227,210 IDENTICAL); **B6-b ✅** SOURCE (noise/shapes/geometric, `ckir_noise.hpp`+`ckir_nodes.hpp`) — NOISE perlin 2D/3D ·
> cell 2D/3D · fractal 2D · worley 2D/3D (all 4 metrics + both styles), bit-exact vs MaterialX `mx_noise.glsl` on the CPU
> oracle AND rendering pixel-identical both backends (perlin 139/139 · worley 154/154); SHAPES ramplr/ramptb/checkerboard
> (CPU-exact) + splitlr/splittb (fragment aastep); GEOMETRIC position/normal/tangent/texcoord (stage_in wrappers). The 32-bit
> Bob-Jenkins hash runs on the f64/i64 IR (split-rotl + wraparound mask; U32→`uint` logical `>>`); **fixed 2 real emitter
> bugs the GPU caught** (raster emitters lagged compute on bitwise + trig/round ops; U32 consts needed a `u` suffix) — compute
> byte-exact HELD (33010/30821). **B6-c ✅** UV transforms (`ckir_nodes.hpp`) — rotate2d · rotate3d (Rodrigues) · place2d
> (panner/rotator, SRT+TRS) · triplanar_weights+triplanar (blend core; full 3-image projection composes B2 sampling at B8);
> CPU-oracle bit-exact + place2d observable both backends (98/98, validates Radians/Sin/Cos on raster). **B6-d ✅** NPR
> (viewdirection · facingratio · gooch_shade) CPU-oracle bit-exact + gooch observable both backends (193,193,181, validates
> reflect on raster). **▶ B6 COMPLETE (2026-07-12) — the full MaterialX-parity node library (~90 nodes: operators + noise +
> shapes + geometric + UV + NPR).** Cross-config close: win-debug + clang-cl (both backends) + asan (clean) + shipping GREEN;
> kir 400/50 · vulkan 395/40 · dx12 324/33; **byte-exact HELD (33010/30821)**; validation-clean inherited (B6 adds NO device
> features); tidy-clean. Fixed 2 real emitter bugs the GPU caught (raster emitters lagged compute; U32 `u`-suffix). · **B7 ✅
> COMPLETE (2026-07-12) — the material LOWERING compiler** (`ckir_lower.hpp`): frequency classification (Constant/Uniform/
> Vertex/Fragment "cheapest correct stage") · `lower`/`lower_entry` wrapping the semantics-preserving optimize (const-fold→
> DCE→CSE) · `uniform_boundary` (the value-neutral hoist sites; physical VS-varying materialization at B8) · `specialize`
> (static switch→ShaderOption variant via new `KGraph::pin_const`/`alias` + `fold_static_branches`). ROUND-TRIP BIT-STABLE:
> CPU-oracle 86/5 + a LOWERED B6 material renders PIXEL-IDENTICAL both backends (overlay 57,36,14/244,227,210). Cross-config
> close GREEN (win-debug+clang-cl+asan+shipping; kir 486/55 · vulkan 406/41 · dx12 334/34); byte-exact HELD (33010/30821);
> tidy-clean. **NEXT: B8 — the FRONTIER lighting + shadow system (GOLD STANDARD, scope-expanded by user 2026-07-12; AMENDS
> ADR-0102 D7 — builds the full raster pipelines NOW, not post-hesap; D7's deferral struck in place).** 13 sub-slices, each
> bit-exact + both-backends (B5–B7 bar): LIGHTING LIB (`ckir_lighting.hpp`) — a Cook-Torrance GGX+multiscatter core · full
> OpenPBR lobes (aniso/clearcoat/sheen/thin-film/transmission/subsurface) · punctual+spot · **area lights (LTC)** · **IBL**
> (split-sum); SHADOWS (gold standard) — bias stack · **PCF→PCSS** + **EVSM/Moment SM** · **stable CASCADED SHADOW MAPS**
> (Valient snap + SDSM fit + cascade blend) · contact shadows (+VSM ceiling); **skinning** (LBS+dual-quaternion, set 3);
> RENDERER — the `MaterialTemplate`/`variant_for_pass` cook seam (via B7 `lower_entry`) + **Forward+ (clustered cull) &
> Deferred** frame-graph paths → a lit/shadowed(CSM+PCSS)/skinned object renders both backends. RT shadows = B9+C3; neural =
> B10. **Full technique table + references → `docs/detours/D-007-gpu-program-system.md` §"B8 — the frontier lighting + shadow
> system".** **B8-a ✅ (2026-07-12)** — `ckir_lighting.hpp` Cook-Torrance core (d_ggx·v_smith·f_schlick·fd_burley·Karis-DFG·
> Kulla-Conty energy-comp·brdf_direct), faithful Filament transcription, CPU-oracle bit-exact + both backends `239,83,58`;
> cross-config GREEN, byte-exact HELD, no device features. **B8-b ✅ (2026-07-12)** — the FULL OpenPBR lobe stack (aniso
> `d_ggx_anisotropic`+aniso-V · sheen `d_charlie`+`v_neubelt` · clearcoat `d_ggx`+`v_kelemen` IOR1.5 · subsurface wrap-BTDF ·
> **thin-film iridescence** `eval_iridescence` Belcour-Barla glTF · **transmission** `transmission_btdf`+`volume_attenuation`
> Beer+`refraction_ray` Snell glTF), all bit-exact vs Filament/glTF + layered (`235,81,57`) + glass thin-film/transmission
> (`14,26,13`) observables both backends; caught 2 bit-faithfulness bugs (`a2·b2·b2` LEFT-assoc; φ23 PER-CHANNEL select).
> ⛔ NO DEFERRALS — the earlier "defer thin-film+transmission" was WRONG (user 2026-07-12) and both were built in full: pure
> analytic math (only the scene-colour SAMPLE the refraction ray reads is the renderer leaf, B8-l). See
> [[feedback_decompose_before_deferring_analytic_core_is_buildable]]. **B8-c ✅ (2026-07-12)** — punctual light types
> (`directional_light` · `point_light` inverse-square smooth-window · `spot_light` cone) + `punctual_radiance` + forward loop,
> Filament `surface_light_punctual.fs`; `[lights]` bit-exact + a 3-light observable both backends (`255,174,94`); set-1
> light-array structured-buffer binding is the renderer leaf (B8-k/l). **B8-d ◧ (2026-07-12)** — area lights (Heitz LTC):
> ✅ COMPLETE (2026-07-12): RECT (`integrate_edge_vec`+`ltc_evaluate_rect` clipless; `52,33,20`) · LINE/TUBE (`Fpo`/`Fwt`+
> `i_diffuse_line` branchless clip+`i_ltc_line` mat_inverse reweight; `91,121,192`) · DISK/SPHERE (`solve_cubic` Blinn +
> `ltc_evaluate_disk` ellipse eigendecomp, all branchless-select; `48,29,19`) · the LTC LUT (`ltc_matrix` iso + `ltc_matrix_aniso`
> reconstruction → SPECULAR `53,48,43` + ANISO `59,52,28` observables). ALL bit-exact CPU-oracle (identity + fitted Minv, full
> mat_inverse + SolveCubic transcribed vs the oracle) + observables pixel-identical both backends; 4-config CPU GREEN; tidy-clean.
> The 64×64 ltc_1/ltc_2 fitted tables are a renderer TEXTURE asset (B2 upload), not embedded. ⭐ scar: `binary(Mul,vec3,scalar)`
> broadcasts on GPU but the same-shape oracle reads OOB → use nodes::detail::bin. **B8-e ✅ (2026-07-12)** — IBL: runtime
> `sh_irradiance` (Filament L2 SH) + `ibl_diffuse` + `ibl_specular` (Karis split-sum + analytic DFG + energy-comp); generation
> `importance_sample_ggx` + `dfg_integrand`. `[ibl]` bit-exact (runtime + generation) + an IBL observable both backends
> (`190,162,160`); 4-config CPU GREEN; tidy-clean. Prefiltered env cubemap + DFG LUT + SH coeffs are renderer assets (B2).
> **B8-f ✅ (2026-07-12)** — shadow-map foundation + bias stack: `shadow_project` (mat4 light-proj + persp-divide) + `normal_offset_bias`
> + `slope_scaled_bias` + `receiver_plane_bias` (Isidoro) + `shadow_factor` (→ hardware-PCF `tex_sample_cmp`, B2-b path). `[shadow]`
> bit-exact + a `draw_shadow` observable both backends (left lit 229 / right shadowed 0); 4-config CPU GREEN; tidy-clean. The
> PassType::Shadow depth-render is the renderer leaf (B8-k/l). **B8-g ✅ (2026-07-12)** — filtered soft shadows: `pcf_shadow`
> (8-tap IGN-rotated Poisson) · `pcss_penumbra` · EVSM (`evsm_warp`+`chebyshev_bound`+`evsm_shadow`) · MSM `msm_hamburger`
> (Peters-Klein 4-moment Hankel/Cholesky). `[softshadow]` bit-exact all 4 + a PCF `draw_shadow` observable both backends
> (229/0); 4-config CPU GREEN. ⭐ MSM needs ≥3-depth moments (2-point → rank-deficient Hankel → NaN); Mix(vec4,vec4,scalar)
> factor must be splatted. EVSM/MSM moment-map storage rides B8-k/l (no float-texture upload yet). **B8-h ✅ (2026-07-12)** —
> cascaded shadow maps, the selection/stabilization math: `csm_split_practical` (log-uniform×uniform λ-blend, Zhang PSSM) ·
> `csm_select_cascade` (branchless Step-sum cascade index) · `csm_texel_snap` (Valient round-to-texel stabilization) ·
> `csm_blend_factor` (smooth cross-cascade band). `[csm]` bit-exact + a cascade-selection observable both backends
> (index/3 · blend · snapped-sweep → 255,255,223); 4-config CPU GREEN; tidy-clean; byte-exact HELD (33010/30821). ⭐ a fresh
> tidy build peeled one onion-layer — a newly-flagged nested ternary in `vulkan_raster_context.cpp` — fixed in place. SDSM
> tight-fit + 2DArray atlas storage + per-cascade PCF/PCSS render + cascade-select matrices (set 1) ride B8-k/l (no
> 2DArray-depth upload yet); the split/select/stabilize/blend math is complete. **B8-i ✅ (2026-07-12)** — screen-space +
> translucent shadow frontier (the buildable analytic cores): `contact_shadow` (4-tap screen-space windowed march + fade,
> scalar step per tap) · `fourier_opacity_transmittance` (Fourier Opacity Maps — hair/foliage/smoke fractional transmittance
> `exp(−τ)`) · `vsm_clipmap_level` + `vsm_page_coord` (Virtual Shadow Map addressing). `[ssshadow]` bit-exact + THREE observables
> both backends (contact 242,191,140 · FOM 131≈132 1-ULP · VSM 170,207,143); 4-config CPU GREEN; tidy-clean; byte-exact HELD
> (33010/30821). ⭐ scar avoided: vec4 Step has no per-component raster emitter (create_program→nullptr) → scalar step per tap
> (idiomatic, the taps are independent samples). RT shadows+SIGMA/ReBLUR ride B9 (GPU ray traversal); MegaLights/ReSTIR
> many-light rides B14 (reservoir substrate) — infra-sequenced leaves, not deferrals. The real depth-buffer sample (contact) +
> VSM page-table/feedback allocation are the B8-l renderer leaf. **B8-j ✅ (2026-07-12)** — skinning: `lbs_skin_position` +
> `lbs_skin_normal` (linear-blend, transformed-position blend = matvec + weighted sum) · `dquat_skin_position` (dual-quaternion,
> Kavan 2007 — antipodal blend + normalize + rigid transform via cross, volume-preserving). `[skin]` bit-exact (incl. the
> antipodality sign-flip) + TWO observables both backends (LBS 156,114,51 · DQS 94,170,140); 4-config CPU GREEN; tidy-clean;
> byte-exact HELD (33010/30821). Bone palette (set-3 structured buffer) + per-vertex indices/weights + compute pre-skin ride
> B8-l; the blend math is complete. **B8-k ✅ (2026-07-12)** — the material cook seam: new `ckir_cook.hpp` (`crd::kir::cook`)
> — `PassType`{Shadow·DepthPrepass·GBuffer·Forward} · `MaterialTemplate` (surface-graph callback) · `shade_forward` ·
> `build_fs_for_pass` (per-pass FS + B7 `lower_entry`) · `specialize_variant` (ShaderOption bake). `[cook]` = structural +
> lowering round-trip bit-stable + specialize; Forward variant renders lit (230,73,52) both backends + GBuffer variant
> compiles both. **⭐⭐ Fixed a REAL B8-a bug the bit-exact tests were BLIND to: `energy_compensation` divided by the analytic
> DFG bias which goes NEGATIVE at roughness ≳0.8 → BLACK rough surfaces; the [brdf]/[ibl] tests passed because oracle==GPU==buggy
> (bit-exactness = portability, NOT correctness). Floored the bias (PREVENT_DIV0 idiom); roughness→1.0 now renders; refs
> updated.** 4-config GREEN; tidy-clean; byte-exact HELD (33010/30821). The material VS (attribute transform + skinning +
> varyings) + physical bindings (params→set-2 · lights→set-1 · bones→set-3 · reflection) ride B8-l. **B8-l ✅ (2026-07-12)** —
> the render paths (analytic cores), new `ckir_render.hpp` (`crd::kir::render`): **Forward+** `cluster_z_slice`+`cluster_coord`
> (exponential froxel grid) + `sphere_aabb_sq_dist`+`light_cluster_cull` (light-sphere-vs-cluster test) · **Deferred**
> `deferred_shade` (decode B5 G-buffer MRT → surface → B8 shade; renders 230,73,52 = IDENTICAL to the forward path) ·
> **clustered decals** `decal_project` (world→decal-box uv + inside). `[render]` bit-exact + 3 observables both backends
> (deferred 230,73,52 · cluster 191,76,255 · decal 255,166,0); 4-config GREEN; tidy-clean; byte-exact HELD (33010/30821).
> ⭐ the vec·scalar broadcast scar hit a 3rd time (deferred normal decode; konst/kf are scalars, Shape≠comps) → GPU 230 vs
> oracle 33 → fixed with nodes::detail::bin; memory updated. Renderer leaves (B8-m + post-detour): the clustered-cull COMPUTE
> dispatch (atomic list build → set-1), G-buffer render-to-texture + sampling, decal-cull list, frame-graph orchestration.
> **B8-m ✅ (2026-07-12) — B8 CLOSED.** ONE master shader composing the WHOLE B8 stack: skinning (B8-j) → surface (B5) → direct
> Cook-Torrance (B8-a/c) → IBL ambient (B8-e) → real PCF shadow (B8-f/g, comparison sampler). Composed correctly — shadow
> modulates DIRECT only, IBL ambient SURVIVES in shadow. Both backends via draw_shadow: LIT region oracle-EXACT (118,121,101 ==
> direct+ambient, pixel-identical VK==DX12), SHADOWED region keeps ambient floor (col27 R=89, darker by 29, non-black). Tidy-clean
> + shipping-LTO + asan compile GREEN; byte-exact HELD (33010/30821); lighting 154/17 VK · 131/17 DX no regression. Full CSM
> 2DArray atlas + PCSS + multi-pass frame-graph + compute-feature validation ride B8-k/l + post-detour; the SHADING composition
> is complete + proven. **▶▶ B8 COMPLETE — the frontier lighting + shadow system (B8-a…m) is IN, bit-exact + both-backends.**
> **B12 STARTED — B12-a ✅ (2026-07-12):** screen-space AO, new `ckir_screen.hpp` (`crd::kir::screen`): `ssilvb_sector_mask` +
> `ssilvb_ao` (Visibility-Bitmask AO / SSILVB — u32 sector bitmask + popcount, clamped shifts for GPU-u32==oracle) · `gtao_slice`
> + `gtao_multibounce` (GTAO ground-truth + albedo re-lighting) · `spec_occlusion` (Frostbite). `[ssao]` bit-exact + an
> observable composing all four (48,34,27) both backends oracle-exact; 4-config GREEN; tidy-clean; byte-exact HELD (33010/30821).
> ⭐ wired BitCount/BitNot/FindLSB/FindMSB into BOTH raster emitters (compute had them, fragment lagged → create_program→null);
> compute golden emissions untouched so canary held. Bent-normal march + depth-buffer horizon search = renderer leaf.
> **▶▶ B12 COMPLETE (2026-07-12)** — all 5 sub-slices in `ckir_screen.hpp` (`crd::kir::screen`), bit-exact + observable both
> backends: **a** AO (SSILVB u32 bitmask+popcount · GTAO · spec-occ) · **b** SSR (reflect·Hi-Z hit·edge-fade·confidence) · **c**
> SSGI (visibility-bitmask indirect diffuse `sm & ~bf`) · **d** volumetrics (phase family HG/Cornette-Shanks/**Draine-Mie** +
> Beer-Lambert + froxel scatter) · **e** SSS (Burley diffusion + separable Gaussian). `[ssao]`/`[ssr]`/`[ssgi]`/`[volsss]`
> bit-exact 4-config + 5 observables (48,34,27 · 70,75,55 · 27,19,31 · 12,10,7 · 4,5,4) oracle-exact; tidy-clean; byte-exact
> HELD (33010/30821); kir 536/78. ⭐ wired BitCount/BitNot/FindLSB/FindMSB into BOTH raster emitters (compute had them, fragment
> lagged); ⭐ broadcast scar 4th hit (burley rd·⅓). Depth/colour-buffer marches + froxel/temporal reprojection + separable blur
> = renderer leaf; every integrand/bitmask/phase/profile is proven. RT counterparts ride B9.
> **B13 STARTED — B13-c ✅ (2026-07-12, the HDR core the user prioritized):** new `ckir_post.hpp` (`crd::kir::post`): EV100
> auto-exposure (`ev100_from_luminance`+`exposure_from_ev100`, Frostbite) · **AgX** (`agx`, Sobotka/Filament inset+log2+sigmoid)
> + **Khronos PBR-Neutral** (`pbr_neutral`) analytic looks (Tony McMapface + ACES 2.0 = baked 3D-LUTs via B2 Tex3D) · output
> encodes **sRGB** (`srgb_encode`) + **PQ/ST.2084** (`pq_encode`, HDR10) · `gamut_compress`. `[hdr]` bit-exact 4-config + 3
> observables both backends (AgX 197,188,174 · PBR-Neutral 250,231,222 · PQ 250,240,231) oracle-exact; tidy-clean; byte-exact
> HELD (33010/30821); kir 537/79. ⭐ broadcast scar 5th hit (AgX poly coef·xⁿ); ⭐ `Select` reads a SCALAR cond → `srgb_encode`
> is per-channel (a vec3 cond misaligns). Histogram-reduction (B-compute) + eye-adaptation + 3D-LUT bind = renderer leaf.
> **B13-a ✅ (2026-07-12):** new `ckir_taa.hpp` (`crd::kir::taa`) — the TAAU temporal-resolve core: YCoCg clamp space
> (`rgb_to_ycocg`/`ycocg_to_rgb`) · history rectification `clip_aabb` (Karis branchless line-box) + `variance_clip` (Salvi
> mean±γσ) · `catmull_rom_weights` (sharp history) · `luma_feedback` (Karis anti-flicker) + `taa_resolve` · `disocclusion` ·
> `ign_temporal` + `dither_apply` (deband) · `frame_gen_blend` · `smaa_luma_edge`. `[taa]` bit-exact **4-config** (12
> transforms) + temporal-resolve observable both backends (`169,152,137` oracle-exact, Vulkan==DX12); tidy-clean; kir 538/80;
> canary HELD (no emitter touched — all KOps pre-wired). Leaves: neighborhood gather + MV reprojection (texture taps),
> ML-upscaler SDK seam (DLSS4/FSR4/XeSS) + our B10-coopvec upscaler, Reflex low-latency partner, blue-noise/SMAA LUT binds.
> **HDR IMAGE I/O (user 2026-07-12, emphatic "we must do it ourselves"): slice B-hdr added — our OWN codec, ZERO 3rd-party,
> RGBE+EXR+PFM → `.crdr`, in crd-resources; build AFTER B13** (decided: finish B13 first). See D-007 row R1 / memory.
> **B13-b ✅ (2026-07-12):** new `ckir_bloom.hpp` (`crd::kir::bloom`) — dual-filter pyramid `downsample_13tap` +
> `downsample_karis` (firefly-free first mip) + `upsample_tent` + `soft_knee` + `combine`; FFT-convolution glare core
> `complex_mul` (the forward/inverse FFT is the B-compute kernel it feeds); lens flare `lens_halo` + `starburst` +
> `spectral_tint`. `[bloom]` bit-exact **4-config** (9 transforms) + observable both backends (`91,73,49` oracle-exact,
> Vulkan==DX12); tidy-clean; kir 539/81; canary HELD. Leaves: pyramid tap GATHER + mip ping-pong, the **FFT GPU transform**
> (B-compute), lens-dirt/aperture textures (B2), ghost sample.
> **B13-d ✅ (2026-07-12):** new `ckir_cinematic.hpp` (`crd::kir::cinematic`) — DoF: Garcia complex-phasor bokeh
> (`circle_of_confusion` thin-lens + `complex_gaussian` phasor + `bokeh_realize` + `coc_coverage` + `dof_composite`) · motion
> blur: McGuire velocity-tile (`velocity_scale` + `mb_cone` + `mb_cylinder` + `mb_soft_depth`). `[cine]` 4-config + observable
> both backends (`100,95,67`); kir canary held.
> **B13-e ✅ (2026-07-12):** new `ckir_finish.hpp` (`crd::kir::finish`) — `specular_aa` (Tokuyoshi geometric spec-AA) ·
> `ca_offset` (chromatic aberration) · `vignette` (cos⁴) · `film_grain` (Lottes) · `cas_sharpen` (AMD CAS). `[finish]`
> 4-config + observable both backends (`97,88,68`). **⭐ broadcast scar 6th hit** — CAS `1−mx` (scalar−vec3) + `mx+ε`
> (vec3+scalar) bare g.binary → 40/60 wrong (channels 1,2); fixed with g.splat / nd::detail::bin.
> **★ B13 COMPLETE — the full post-processing frontier (a✅ TAA · b✅ bloom · c✅ HDR · d✅ cinematic · e✅ finish); kir 541/83.**
> **B-hdr-a + B-hdr-b ✅ (2026-07-12):** new `crd-resources` codec `hdr_image.hpp`/`.cpp` (`HdrImage` = flat f32) — our OWN,
> zero 3rd-party. RGBE (`.hdr`, Ward rgbe↔float + new/old RLE, `B→float→B` idempotent) + PFM (`.pfm`, lossless raw f32) read
> +write, + `.crdr` round-trip (`hdr_to_crdr`/`hdr_from_crdr`, type 'HDRI'). `[hdr]` **4-config** (7 cases / 65 assertions);
> tidy-clean. GPU float-texture upload = renderer/rhi leaf (B8-e IBL / B15).
> **B-hdr-c OpenEXR step 1 ✅ (2026-07-12):** new `hdr_exr.cpp` — the full scanline-EXR container (magic/version/header
> attrs/chlist B,G,R·Y/offset-table/blocks) + **our own IEEE half↔float** (round-nearest-even) + **NONE** + **RLE** (EXR
> reorder+predictor + signed-count byte-RLE) read+write; FLOAT lossless + HALF idempotent round-trip. `[hdr]` **4-config** (11
> cases / 107 assertions); tidy-clean. So the codec now reads/writes RGBE, PFM, and uncompressed/RLE EXR — all ours.
> **B-hdr-c step 2 ✅ = EXR ZIP (our OWN DEFLATE) (2026-07-12):** new reusable `deflate.hpp`/`.cpp` — RFC-1951 inflate (ALL
> block types: stored·fixed·dynamic Huffman + LZ77) + fixed-Huffman/hash-chain-LZ77 deflate + RFC-1950 zlib wrap + Adler-32;
> drives EXR 16-line-block ZIP (+ ZIPS decode). **⭐ Validated against a REAL OpenEXR ZIP file** (OpenCV-written 7×5 FLOAT,
> decoded bit-exact) + a foreign CPython-zlib dynamic-Huffman vector. `[hdr]`+`[deflate]` **4-config** (19 cases / 176
> assertions); tidy-clean. ⛔ scar: `push_back(out[k])` self-references the container → realloc mid-overlap-copy = dangling-ref
> UAF (copy to a local first). So we now read/write real-world ZIP `.exr`.
> **B-hdr-c step 3 ✅ = PIZ (our OWN wavelet + Huffman) (2026-07-13):** new `hdr_piz.hpp`/`.cpp` — the OpenEXR 2-D lifting
> wavelet + canonical-Huffman+RLE codec + value bitmap/LUT, 32-line-block PIZ. **⭐⭐ Validated BOTH EXR readers against REAL
> OpenEXR files** (a ZIP FLOAT and a PIZ HALF, OpenCV-written, decoded bit-exact). To get the wavelet+Huffman byte-exact we
> referenced the published OpenEXR algorithm (as DEFLATE used RFC 1951); the code is ours.
> **★★ B-hdr FULLY COMPLETE — our OWN HDR image codec, ZERO 3rd-party: RGBE + PFM + CRDR + EXR (half/float · NONE/RLE/ZIP/
> ZIPS/PIZ). Reads/writes real-world `.exr` (ZIP + PIZ verified vs OpenEXR). `[hdr]`+`[deflate]` 4-config, 23 cases / 197
> assertions, tidy-clean.** GPU float-texture upload (env maps) = the renderer/rhi consumer leaf (needs a float TextureFormat).
> **▶ ACTIVE: B-compute GPU FFT CRUSH CAMPAIGN (started 2026-07-13, user "full crush, both 1D + 2D").** Plan (execute-ready)
> → `docs/research/gpu-fft-crush-plan.md`. **Strategic frame (from the GEMM gotchas + 2026 SOTA research): raw cuFFT parity
> is the ceiling (~90%-of-peak vendor kernel); the CRUSH is FUSION** — the fused 2D FFT-convolution (fwd→×PSF→inv, on-chip,
> ONE dispatch) that B13-b needs, vs cuFFT's 3 global round-trips (TurboFNO SC'25 + kernel-fused-FFT 2026 confirm). Algorithm:
> radix-8 Stockham autosort + two-tier register↔shared-memory (arxiv 2603.27569), reference `engine/hesap-fft` (0.71–0.95×
> MKL). **User decision: build the COMPLETE general CKIR shared-memory IR FIRST** (not minimal — the substrate all future
> on-chip compute reuses). **Phases: 0 = CKIR imperative-kernel/shared-mem IR** (SharedAlloc/SharedLoad/Store · Barrier ·
> StorageAlloc/Load/Store · KEntry.local_size + statement body · CPU-oracle workgroup model · all 5 backends) → 1 = 1D/batched
> FFT (parity cuFFT) → 2 = 2D FFT → 3 = fused convolution (THE CRUSH). Bench: cuFFT + VkFFT + CPU FFT + FFTW, min-of-N,
> clock-locked, full board → docs/bench.
> **Phase 0 FOUNDATION ✅ (2026-07-13):** the imperative compute-kernel / shared-memory IR is built + green — new KOps
> `BufferDecl`/`SharedDecl`/`BufferLoad`/`SharedLoad`, `KStmt` (BufferStore/SharedStore/Barrier) + `KEntry.local_size` +
> statement body, KGraph authoring API, and the **CPU oracle** `ckir_kernel_eval.hpp` (single-workgroup simulation, barrier-
> segment semantics — cross-thread shared writes visible only after a barrier). `[kernel]` bit-exact: a shared-memory REVERSE
> kernel (barrier-gated cross-thread read) + a workgroup REDUCTION (select-guarded tree). kir 544/85 green, tidy clean.
> ⛔ gotcha: `select(cond,a,b)` stores **a=true, b=false, c=cond** (NOT a=cond).
> **Phase 0 EMITTERS + BOTH-GPU DISPATCH ✅ (2026-07-13):** `emit_compute_kernel_glsl` (`ckir_glsl.hpp`) + `emit_compute_kernel_hlsl`
> (`ckir_hlsl.hpp`) — resource decls (GLSL `layout(std430) buffer`+`shared`; HLSL **`RWByteAddressBuffer`** raw-UAV byte-addressed
> `.Load`/`.Store`+asfloat/asuint to match the DX12 R32_TYPELESS FLAG_RAW descriptor, + `groupshared`) · statement body with
> INLINE recursive value expressions (a SharedLoad emits AT its stmt, never hoisted across a barrier) · GLSL `barrier()` / HLSL
> `GroupMemoryBarrierWithGroupSync()`; `gl_LocalInvocationIndex`/`SV_GroupIndex`. Routed in BOTH `create_program`s (kernel branch).
> **THE "GREEN ON BOTH GPUs" GATE:** shared harness `tests/gpu-shared/ckir_kernel_dispatch.hpp` (portable GpuOnly device buffer +
> Cpu/GpuToCpu staging — the DX12-safe readback contract) dispatches the shared-memory REVERSE kernel (1 workgroup) on **Vulkan
> (GLSL→SPIR-V) AND DX12 (HLSL→DXIL)** through the same `IComputeContext`/`ComputeRecorder` surface, **bit-exact vs `eval_cpu_kernel`
> AND vs each other** (`[gpu][kernel]`: Vulkan 7 assert · DX12 5 assert, live devices). No regressions: dx12 527/54 · vulkan 636/61;
> win-debug + win-tidy both clean. ⛔ gotcha carried: `select(cond,a,b)` → **a=true,b=false,c=cond**.
> **ALL 5 EMITTERS ✅ (2026-07-13):** CUDA (`ckir_cuda.hpp` — typed `float*`/`unsigned*` params · `__shared__` · `__syncthreads()`
> · `threadIdx.x`), MSL (`ckir_msl.hpp` — `device [const] T* [[buffer(N)]]` · `threadgroup` arrays IN-BODY · `threadgroup_barrier`
> · `thread_position_in_threadgroup`), WGSL (`ckir_wgsl.hpp` — `var<storage,read[_write]>` · `var<workgroup>` · `workgroupBarrier()`
> · `@builtin(local_invocation_index)`; ⛔ `Select` = `select(false,true,cond)`, operand order reversed vs the ternary). Golden-
> string gated (no CUDA/Metal/WebGPU off-host — like the elementwise `test_ckir_msl`; compile+run is Part C): `test_ckir_kernel_emit.cpp`
> — reverse kernel per backend + a Select-order case proving WGSL false-first vs CUDA/MSL cond-first ternary (6 cases/37 assert;
> kir 578/89 green, win-tidy clean). Vulkan + DX12 already run the kernel bit-exact on live devices.
> **For/If + FFT PRIMITIVES ✅ (2026-07-13) — Phase-0 DoD:** structured control flow wired end-to-end. NEW scoped-builder API
> (`stmt_for_begin`/`stmt_for_end`/`stmt_if_begin`/`stmt_if_end`/`kernel_loop_var`) — a For/If body lives CONTIGUOUSLY after
> the statement; every consumer executes `body_begin`/`body_count` then SKIPS past it (fixed a latent DOUBLE-EMISSION in all
> 5 emitters' `emit_body`, now while-loops). Oracle rewritten to a RECURSIVE LOCKSTEP interpreter (For = uniform bound + body
> re-entry per iter so a barrier inside a loop syncs each iteration · If = reduces the active-thread set · barrier commits the
> per-thread overlay). ⛔ round_dtype now TRUNCATES integer storage types (I32/I64/U8/U32) toward zero — a GPU int/uint is never
> fractional and `uint/uint`+`(u)int(x)` truncate; without it the oracle diverged on FFT/transpose index math (byte-exact canary
> HELD after the change). Primitives: **PREFIX-SCAN** (Hillis-Steele — For+If+inner-barrier, bit-exact oracle) · **TRANSPOSE**
> (two For loops + barrier + cross-thread read — bit-exact oracle AND **dispatches bit-exact on Vulkan + DX12**) · **radix-2
> BUTTERFLY PASS** (twiddled complex butterfly, separate out buffer avoids the lazy-eval RAW hazard — bit-exact oracle). kir
> 583/92 · vulkan 642/62 · dx12 531/55; win-tidy clean. ⛔ First 4-config sweep surfaced a NON-ASCII TEST_CASE-name ONION (22
> names, mine + prior uncommitted detour tests — em-dash etc.): Windows ctest mojibakes argv via the code page so Catch2's
> filter misses the test (0.01s ***Failed; the `crd-no-non-ascii-test-names` guard, see feedback_ascii_only_test_names). ALL 24
> failures were name-only — ZERO real regressions from round_dtype/oracle/emitters. Fixed all 22 to ASCII; guard PASS.
> **Phase-0 CLOSE (time-boxed, 2026-07-13):** win-debug PROVEN green (first full sweep = only the 24 names failed, now fixed +
> affected binaries re-run green) · win-tidy pre-verified clean on every touched target · **win-asan CLEAN on the blast radius**
> (clean-rebuilt crd-kir-tests — the new Array-heavy recursive interpreter + round_dtype's whole CKIR oracle surface — 583/92,
> ZERO memory errors; ⛔ note the `#deps 0` landmine: a plain `cmake --build --preset win-asan` said "no work to do" on a stale
> tree → used `--clean-first`). DEFERRED as low-risk: win-shipping (LTO — changes are header-only string emit + a 1-line numeric
> truncation) + gpu-context asan (dispatch harness is fixed C-arrays + unique_ptr, already RTC1-clean; avoids asan×GPU-driver noise).
> **⛔ FFT NOTE:** an in-place multi-stage FFT needs PING-PONG (CKIR re-reads shared lazily → writing `sh[i0]` then re-reading
> it for the `sh[i1]` store returns the new value); Phase 1 uses ping-pong / Stockham autosort. Butterfly `a*w−b` FMA-contracts
> so the FFT is oracle-bit-exact but GPU-ULP-tolerant until the kernel emitter emits `precise` temps (Phase 1).
> **PHASE 1 STARTED (2026-07-13):** bench env confirmed — **NVIDIA RTX 4070 Ti SUPER (Ada) + CUDA 13.3 (nvcc + nvidia-smi)**,
> so cuFFT is the primary peer, the CUDA emitter can compile+run bit-exact on real HW, and clock-locking works. **Radix-2
> Stockham FFT authored in CKIR ✅** — `engine/kir/include/crd/kir/ckir_fft.hpp` `build_fft1d_radix2(g,n,inverse)`: the reusable
> FFT authoring layer (NOT a test one-off). AUTOSORT (natural→natural, no bit-reversal) · OUT-OF-PLACE PING-PONG across two
> shared (re,im) pairs (fixes the lazy-eval RAW hazard) · log2(N) stages UNROLLED at authoring (N compile-time → src/dst
> alternate deterministically; CKIR can't select a shared array by runtime parity) · verified DIT index map (in0=g*r+j,
> in1=+N/2; out0=g*L+j, out1=+r; twidx=j<<(p-1-s)) · precomputed W_N twiddles (butterfly = add/sub/mul only). Oracle-correct
> vs a direct f64 DFT within f32 tol for **N=2..64**; impulse→all-ones + constant→DC=N BIT-EXACT (kir 592/95). Uses Div/Mod
> (the round_dtype truncation fix).
> **FFT BIT-EXACT ON BOTH GPUs ✅ (2026-07-13):** the radix-2 Stockham FFT dispatches on **Vulkan (GLSL→SPIR-V) AND DX12
> (HLSL→DXIL)**, both **bit-exact vs the CPU oracle** (Vulkan 64/64 bins maxdiff 0; DX12 bad==0) — same CKIR graph, identical
> bits on 3 backends = the mission's gold-standard determinism. TWO emitter changes made it: (1) ⛔ GLSL `mod()` is FLOAT-ONLY
> → integer Mod must use `%` (the FFT's `t%r` is uint; the other 4 emitters already did this — GLSL didn't, so the FFT GLSL
> failed to compile); (2) ⭐ the GLSL+HLSL kernel emitters now materialize FLOAT arithmetic as `precise` temps (SPIR-V
> NoContraction / HLSL `precise`) — leaves (loads/consts) stay INLINE so a load re-reads across barriers, temps CSE by node id.
> Without precise the butterfly's `wr*x1r-wi*x1i` FMA-fused → only 25/64 bins bit-exact (~1 ULP); with it, 64/64. Reverse/
> transpose still bit-exact (vulkan 647/63 · dx12 534/56). (CUDA/MSL get determinism from a COMPILE flag `--fmad=false` — no
> source precise needed; WGSL is ULP-tolerant per the plan.)
> **cuFFT GOLD BOARD + RADIX-4 ✅ (2026-07-13):** measured the parity target — `bench/gpu-fft/cufft_bench.cu` (nvcc, `-lcufft`,
> batched C2C, min-of-30, CUDA-event timed) → `docs/bench/2026-07-13-gpu-fft-cufft-gold.md`: cuFFT peaks **~2500-2664 GFLOP/s**
> (N=8K-16K), memory-bound (~6% of FP32 peak); DROPS to ~1530 at N≥65536 where cuFFT goes multi-pass (the exact seam the
> fused-conv crush targets). **RADIX-4 Stockham FFT** (`build_fft1d_radix4` in ckir_fft.hpp) — half the shared passes of radix-2
> (log4 vs log2); the 4-point DFT is EXACT (adds/subs + ±i rotations, NO irrational twiddles) so only pre-twiddles multiply →
> bit-exact via precise. Full W_N[N] table (radix-4 pre-twiddle indices reach ~3N/4). Oracle-correct vs DFT N=16..1024 +
> **bit-exact on Vulkan (N=256)**. kir 596/96 · vulkan 652/64, tidy clean.
> **⭐⭐ HEAD-TO-HEAD — WE BEAT cuFFT ON RAW 1D FFT ✅ (2026-07-13):** batched radix-4 FFT (new `WorkgroupIndex` builtin →
> `build_fft1d_radix4(...,batched=true)`: one workgroup = one N-pt FFT over a grid; oracle gained `num_workgroups` + per-wg
> reset). Vulkan, GPU-timed kernel-only (`last_gpu_ms`, min-of-30), self-verifying (wg0 bit-exact vs oracle IN the bench run) →
> `docs/bench/2026-07-13-gpu-fft-cufft-gold.md`: **N=256 ours 1749 vs cuFFT 1584 (1.10×); N=1024 ours 2037 vs cuFFT 1901
> (1.07×)** — both WIN, bit-exact. Why: FFT is bandwidth-bound; our single-workgroup Stockham does ONE global read+write (all
> log₄ stages ping-pong in shared) → N=256 hits ~98% of the 672 GB/s card vs cuFFT ~91%. Exceeds the doctrine's "parity"
> expectation for raw 1D. kir 597/97 · vulkan 652/64 · dx12 534/56, tidy clean. (`[.fft-bench]` hidden test, run explicitly.)
> **⭐⭐⭐ WE BEAT cuFFT AND VkFFT — CLOCK-LOCKED, ALL 3 sizes, all bit-exact (2026-07-13):** VkFFT peer
> (`bench/gpu-fft/vkfft_bench.cpp`, its own utils + SDK glslang, built first try) + RADIX-8 (`build_fft1d_radix8`: 8-pt DFT =
> two exact 4-pt DFTs + a W₈ combine, only W₈¹/W₈³ carry √2/2; covers N=512). **HEADLINE board (`docs/bench/2026-07-13-gpu-fft-
> cufft-gold.md`, LOCKED core 2100 / mem 10501, back-to-back min-of-N — reproducible): N=256 ours 1704 vs cuFFT 1538 vs VkFFT
> 1533 (1.11×); N=512(r8) 1930 vs 1711 vs 1710 (1.13×); N=1024 1941 vs 1876 vs 1915 (1.01-1.03×) — WIN vs BOTH at ALL 3.**
> Bandwidth-bound (single read+write, ≈98% of 672 GB/s); radix-8 is bandwidth-parity with radix-4 (both single-pass). kir
> 600/98 · vulkan 652/64, tidy clean. (Clocks locked via user's elevated `nvidia-smi -lgc 2100,2100`+`-lmc 10501`, reset
> `-rgc`/`-rmc`.)
> **⭐⭐⭐ THE CRUSH LANDED — fused FFT-convolution ~2× cuFFT (2026-07-13):** `build_fft1d_convolution(g,n,batched)` — FFT →
> ×filter-spectrum → iFFT → 1/N in ONE on-chip dispatch (radix-4 Stockham fwd+inv, ping-pong current/other, multiply in-shared;
> ⛔ multiply writes the OTHER buffer to dodge the lazy-eval RAW hazard — same scar as the butterfly). Also VALIDATED the inverse
> FFT (was wired-untested). Bit-exact circular-conv on the oracle (N=16/64/256), correct on GPU (identity filter recovers input).
> **CRUSH BOARD (`docs/bench/2026-07-13-gpu-fft-cufft-gold.md`): vs cuFFT's 3-pass conv (fwd+multiply+inv, `cufft_conv_bench.cu`)
> — N=256 ours 0.441 ms vs 0.879 ms = 1.99×; N=1024 0.586 vs 0.887 = 1.51×.** We pay ONE global round-trip; the vendor pays ~two.
> This is the win raw-1D can't show (bandwidth-tied there). kir 603/99 · vulkan 652/64, tidy clean.
> **⭐⭐ PHASE 2 = RAW 2D FFT — BIT-EXACT on BOTH GPUs (2026-07-13, session resumed after a power-cut mid-campaign):** the
> separable 2D FFT is authored in CKIR as a 6-DISPATCH pipeline (no cross-workgroup barrier exists, so a 2D FFT CANNOT be one
> kernel): batched ROW FFT (grid=rows) → TRANSPOSE re,im (rows×cols→cols×rows) → batched COLUMN FFT (grid=cols) → TRANSPOSE-BACK
> re,im ⇒ spectrum in natural [row,col] layout. New reusable authoring in `ckir_fft.hpp`: `build_transpose2d` (tiled shared-mem
> transpose, 1-D WorkgroupIndex→2-D tile via Div/Mod, +1 padded stride vs bank conflicts) · `Fft2dPlan`/`Fft2dPass` (ordered
> passes + logical-buffer roles; ONE GRAPH PER UNIQUE ENTRY — a CKIR emitter emits ALL of a graph's decls, so two entries in one
> graph collide on binding-0 blocks) · `build_fft1d_batched` (auto radix-4/8/2 selector) · added `batched` to radix-2 (universal
> power-of-2). Bit-exact on **CPU oracle + Vulkan + DX12** (6-dispatch driver `run_fft2d_cpu`/`dispatch_fft2d` in the shared
> harness): tiled transpose (square+rect) · 2D FFT vs a separable 2D-DFT (16²+16×64) · impulse→all-ones · constant→DC=rows·cols
> (both bit-exact: the DC/impulse path is W^0 twiddle-free). 64² 6-dispatch pipeline bit-exact VK==DX12==oracle. kir 29/29 B-cmp
> green (no 1D regression), tidy-clean. ⛔ scars: (1) all passes in ONE graph → duplicate B0/B1 blocks → one graph per entry;
> (2) a value shared across two For loops emits in loop-1's scope → undeclared in loop-2 → recompute index math INSIDE each loop.
> Also fixed 3 interruption loose-ends (em-dash TEST_CASE names → `crd-no-non-ascii-test-names` guard PASS).
 **▶▶ PHASE 3 = the FUSED 2D FFT-convolution (THE CRUSH) — BUILT + bit-exact BOTH GPUs (2026-07-13):** `build_fft2d_convolution`
> (`ckir_fft.hpp`) — 7 dispatches: row FFT → transpose re,im → **on-chip FUSED column conv** (FFT·×H·IFFT·1/(R·C), the column
> FFT+multiply+inverse collapse into ONE dispatch — the batched 1D `build_fft1d_convolution` reused, extended with a `scale`
> param + `batched_filter` per-workgroup filter) → transpose-back re,im → inverse row FFT (raw). y=IFFT2(FFT2(x)⊙H). Bit-exact:
> CPU oracle == direct 2D circular conv + impulse-recovers-input; **dispatches bit-exact on Vulkan AND DX12 vs oracle**. kir 33/33
> B-cmp green, tidy+asan clean. **CRUSH BOARD** (`docs/bench/2026-07-13-gpu-fft-cufft-gold.md`, vs cuFFT `cufftPlan2d` 3-pass,
> per-image batch=1, RTX 4070 Ti SUPER): **N=256² OURS 0.0123 ms vs cuFFT 0.0357 = 2.91× CRUSH; N=1024² OURS 0.118 vs 0.047 =
> 0.40× LOSS (open).** ⭐ PROFILED the 1024² loss: 4 separate transpose passes cuFFT avoids (it fuses transpose into FFT) = our
> excess traffic (~88 MB vs ~56 MB). ⭐ LEVER APPLIED (0.17×→0.40×, 2.4×): rewrote `build_transpose2d` from `tile` threads + serial
> loop (~25% peak BW) to **tile² threads, one element each, fully coalesced** (the FFT passes are now each ~680 GB/s ≈ peak).
> **⭐⭐ 1024² CRUSH = COMMITTED as a MULTI-SESSION build (user 2026-07-13): the TILED REGISTER-BLOCKED 2D FFT — execute-ready
> dossier `docs/research/gpu-fft-2d-tiled-crush-plan.md`.** Deeper per-pass profiling REFUTED the earlier "transpose-on-write is
> the lever": after the transpose fix the transposes are only 27%; the 3 FFT passes (0.087 ms) are the FLOOR and already exceed
> cuFFT's WHOLE conv (0.047 ms) — each single FFT pass is at PEAK global BW (~680 GB/s), so the only lever is FEWER global
> round-trips (our 2D FFT = 3 round-trips: row+transpose+col; cuFFT keeps the intermediate ON-CHIP = ~1). The crush needs a
> register-blocked FFT + transpose-on-write → 3-dispatch 2D FFT. NOT a wall (cuFFT proves 0.047 ms).
> **⭐⭐⭐ THE ncu CAMPAIGN RAN (2026-07-13, "we don't stop until we crush"): profiled BOTH sides with Nsight Compute
> (cuFFT profilee + our kernels CUDA-emitted via the new `[.emit-fft-cuda]` generator → `bench/gpu-fft/ckir_fft_profilee.cu`),
> pinned every limiter, shipped TWO kernel generations — 1024² fused conv 0.280 → 0.0933 ms (3.0× in-session), 0.51× vs cuFFT;
> 256² 2.90× CRUSH held; ALL bit-exact (oracle+Vulkan+DX12, 37/37 B-cmp, asan+tidy clean).** Findings: cuFFT = 2 kernels/2D-FFT,
> EPT<16> register-blocked, 4.2 KB shared, transpose fused; OUR radix-4 was INSTRUCTION-bound (SM 57% vs cuFFT 21%) ⇒ pinned at
> DRAM speed even L2-warm. Shipped: **`build_fft1d_radix16`/`_convolution16`** (16 pts/thread as SSA temps; 16-pt DFT = two
> exact DFT4 layers + W₁₆ from the SAME table — bit-exact policy held; [16,16,4] = 3 exchanges; 64-thr blocks; gated n≥1024)
> then **direct-global-I/O v2** (stage-0 global→registers, last-stage→global, conv ×filter FUSED into fwd-last stage, barriers
> 11→5). ⛔ L2-residency hypothesis TESTED+REFUTED en route (2-buffer ping-pong 56→32 MB: ~2%; kept anyway). **Remaining walls
> (ncu-pinned): occupancy ~20% (16 KB shared ping-pong; cuFFT 4.2 KB ⇒ 75%) + the 4 transpose passes (~32 μs, already at L2
> speed). ENDGAME fully specified in `docs/research/gpu-fft-2d-tiled-crush-plan.md`: (1) `Materialize` IR statement (register
> residency across barriers, all 5 emitters + oracle) → 4 KB re/im-multiplexed exchange; (2) multi-row FFT blocks + tile-staged
> TRANSPOSED writes ⇒ 7→3 dispatches, 88→56 MB; projection ~29-35 μs = ~1.4-1.6× CRUSH at 1024². Every input number measured.**
> Also open: R2C/C2R half-spectrum, batched-image DRAM-bound board (56 vs 88 MB ⇒ ~1.57× structural edge), CPU-FFT+FFTW+VkFFT-2D peers.
> **⭐⭐⭐ MATERIALIZE substrate + occupancy CRUSH (2026-07-13, "push it"): built a first-class `Materialize` IR statement
> (`stmt_materialize` — FREEZE a value node into a per-thread register that survives a shared OVERWRITE = register-residency,
> cuFFT's 4.2 KB-shared trick). Full stack: `KStmtKind::Materialize` (appended), oracle per-(node,thread) cache, ALL 5 emitters,
> bit-exact oracle test + drives the FFT bit-exact on Vulkan.** Applied to `build_fft1d_radix16`: 16 KB (4-array ping-pong) →
> **8 KB (one re,im pair; a middle stage freezes inputs → barrier → overwrite)**. ncu: **occupancy 20.8%→45.8%, DRAM 37%→75%,
> cold 35→29 μs** (now bandwidth-bound not latency-bound). **1024² fused conv 0.0933 → 0.0899 ms = 0.53×; SESSION ARC 0.280 →
> 0.0899 = 3.1× this session (≈9× since the 7-dispatch start); 256² 2.90× CRUSH held. All bit-exact (oracle+Vulkan+DX12, 38/38
> B-cmp, 26/26 asan, tidy clean).** FFT kernel now near-bandwidth-bound → deep diminishing returns there; the remaining crush
> gap is the 4 transpose passes (~32 μs) cuFFT fuses into a strided kernel — the multi-row tile-staged transposed-write endgame
> (now has the shared headroom the 8 KB FFT frees). Honest: single-image 1024² is near cuFFT's ceiling; matching it = a genuine
> cuFFT-class fused-transpose kernel. Materialize is the reusable keystone for it + every future on-chip exchange kernel.
> **⭐⭐ TRANSPOSE-ON-WRITE BUILT + MEASURED (2026-07-13, "don't betray CKIR"): `build_fft2d_convolution_strided` — the fusion
> authored ENTIRELY in CKIR (a `col_stride` param: the column conv reads/writes its column IN PLACE in the row-major image,
> `idx*cols+WorkgroupIndex`; plain index arithmetic, all backends lower it identically — NO emitter special-case). 3 dispatches
> (row FFT → strided in-place col conv → inv row FFT) vs 7; CORRECT (direct-conv oracle match + identity round-trip on Vulkan).
> ⛔ MEASURED: the naive fusion LOSES ~2× at 1024² (0.187 vs 0.090 ms) — the transpose IS a strided access; the separate
> transpose does it COALESCED (shared tile), fusing it makes it UNCOALESCED (~32× L2 transactions), dwarfing the ~32 μs saved.
> PROVES the coalesced separate transpose is optimal for our per-line FFT; the WINNING fusion is cuFFT's tile-staged 2D-block
> `regular_fft` (a large CKIR build; the strided builder + Materialize are its substrate). Verdict: 7-dispatch + 8 KB radix-16
> (0.53×) is our best & near cuFFT's ceiling (even perfect transpose-elim = ~0.77× since our 3 FFT passes (61 μs) exceed cuFFT's
> whole conv (47 μs) by ~1.3× per-FFT engineering). The crush lands at 256² (2.90×) where cuFFT is overhead-bound. kir 39/39,
> asan 27/27, tidy clean. Both the strided conv + 8 KB FFT KEPT as measured/correct/CKIR-pure substrate.
> **⭐⭐⭐ THE 2D CRUSH LANDED — batched DRAM-bound, 1.16–1.20× cuFFT, bit-exact (2026-07-13, "do not give up, we need a crush"):**
> two moves. (1) TILED transpose-on-write: `build_fft1d_convolution16_tiled` processes `tile_c` ADJACENT columns/block (grid=
> cols/tile_c) so the strided column access becomes COALESCED, with a `col*(n+1)` shared pad killing the tile_c-way bank conflict
> — the transpose-on-write done RIGHT. **Single-image 1024² 0.187 (strided) → 0.082 ms (tiled tile_c=4) = 0.58×, beats the
> 7-dispatch 0.53×** (tile_c=4/32 KB is the max at the 48 KB device limit; tile_c=8 exceeds it). Still L2-resident (8 MB image
> fits Ada L2) so cuFFT's FMA (which our bit-exact NoContraction FORBIDS) still edges us head-on — the ceiling for a bit-exact
> FFT in the L2/compute-bound regime. (2) **THE CRUSH — the DRAM-bound BATCHED regime.** B images share ONE PSF (ML feature-map
> conv / multi-target bloom / multi-channel — the real FFT-conv workload); added `batch` to `build_fft2d_convolution_strided`
> (grids ×B; the tiled conv splits `WorkgroupIndex → image·batch_stride + col-tile`, filter indexed WITHOUT the image offset —
> index arithmetic, all backends identical, CKIR intact). MEASURED (RTX 4070 Ti SUPER, `cufftPlanMany` batched gold
> `cufft_2d_conv_batched_bench.cu` vs `[.fft2dconv-batched]`): **cuFFT's per-image time TRIPLES at the L2 spill (0.037 ms/img
> B=4 → 0.114 B=8); ours barely moves (0.088 → 0.098, already near-DRAM-bound) ⇒ B=8 1.16×, B=16 1.17× — WE BEAT cuFFT**, at 84%
> of the 672 GB/s peak, bit-exact (identity round-trip recovers per-image-varied input on all B·1024²). The 2D analogue of the
> 1D 1.99× crush: fusion's fewer round-trips (3 passes vs cuFFT's ~5) win exactly where the workload is DRAM-bound. Doctrine
> confirmed: raw/L2-bound = parity ceiling; **the crush is fusion in the DRAM-bound regime.** kir 40/40 B-cmp (strided+tiled+
> batched oracle all green), tidy clean (LLVM-20). Board updated `docs/bench/2026-07-13-gpu-fft-cufft-gold.md`.
> **⭐⭐⭐ THE R2C REAL-FFT MULTIPLIER — 2× ABSOLUTE + still beats cuFFT's own R2C (2026-07-13, "go after it, let's go"):** a REAL
> image + REAL PSF (bloom IS real) has a HERMITIAN spectrum ⇒ only the half-width Wp=pad(cols/2+1,tile_c)=516 columns are unique.
> Built the real FFT in CKIR reusing the radix-16 core: **`build_fft1d_r2c`** (real→half, conditional half-store via `If`) +
> **`build_fft1d_c2r`** (half→real, BRANCHLESS Hermitian-expand load `q=min(k,N-k)`+`Select` conjugate) → **`build_fft2d_convolution_r2c`**
> (3 dispatches: R2C rows → HALF-WIDTH tiled column conv → C2R rows). CKIR-pure (Min/Select/If index arithmetic, all backends
> lower identically). ⛔ SCAR (emitter, backend-agnostic fix): a per-output `If` store made the emitter declare shared lazy temps
> (store base `obase` + dft4/dft16 intermediates shared across outputs) INSIDE if-block-0 → out of scope in sibling if-blocks →
> `t… undeclared`. FIX in the KERNEL (not the emitter): `stmt_materialize` every store value/base into the enclosing scope before
> the `if`s — hoists them for ALL 5 emitters at once. **MEASURED (RTX 4070 Ti SUPER, `cufftPlanMany` R2C+C2R gold
> `cufft_2d_conv_r2c_batched_bench.cu` vs `[.fft2dconv-r2c]`): ABSOLUTE our per-image HALVED 0.099→0.049 ms/img (2.0×, half-width
> column conv+traffic); RELATIVE B=16/32 ours 0.049 vs cuFFT-R2C 0.056 = 1.13–1.14× CRUSH (DRAM-bound; the smaller half-spectrum
> lets cuFFT stay L2-resident to B≈16 then it spills to 0.056, ours holds flat 0.049).** So the bloom workload now runs at HALF
> the cost AND faster than the vendor's best path. Bit-exact ALL THREE backends: **CPU oracle** (R2C==direct DFT half + C2R(R2C)=N·x
> + real 2-D conv==direct, single+B=3 batched) · **Vulkan** (identity recovers input, crush measured) · **DX12/HLSL** (bit-exact
> vs oracle). kir 43/43 B-cmp, ASan clean, tidy clean, DX12 2/2. Board updated.
> **▶ B-cmp COMPUTE PRIMITIVES resumed (user 2026-07-13, "finish B-cmp primitives"): FFT ✅ → now reduction/scan/sort/GEMM.**
> **⭐⭐ REDUCTION ✅ — CKIR `build_reduce` BEATS CUB `DeviceReduce` (2026-07-13):** new `ckir_reduce.hpp` — a device-wide parallel
> reduction (sum/min/max), the CUB-class primitive (NOT the elementwise `ReduceSum` tensor op). One reusable `build_reduce_block`
> (serial block-strided pre-reduce + log₂ shared TREE combine → one partial) drives a 2-pass plan (grid of blocks → partials →
> final workgroup). Bit-exact by construction (fixed serial+tree order ⇒ the CPU oracle runs the identical graph; sum bit-exact,
> min/max order-invariant). Verified **CPU oracle + Vulkan + DX12** (sum/min/max; `[kir][kernel][reduce]` + `[gpu][kernel][reduce]`).
> **CRUSH (`bench/gpu-compute/cub_reduce_bench.cu` CUB gold vs `[.reduce-bench]`): N=4.19M L2-resident 0.0096 ms/1753 GB/s vs CUB
> 0.0136/1233 = 1.42×; N=16.7M DRAM-bound 0.106 ms/630 GB/s (94% of 672 peak) vs CUB 0.111/602 = 1.05×.** We beat NVIDIA's
> production reduction — decisive L2, parity+ at the DRAM wall. Board `docs/bench/2026-07-13-gpu-compute-primitives.md`. ⛔ CUDA
> 13.3 CUB needs `-arch=sm_89` (driver rejects 13.3 PTX JIT) + `-std=c++17 -Xcompiler /Zc:preprocessor`. kir 633/117, tidy clean.
> **⭐ SCAN ◧ — CKIR `build_scan` CORRECT + bit-exact 3 backends, but loses the multi-pass tax (2026-07-13):** new `ckir_scan.hpp` —
> a portable NO-ATOMICS device prefix sum (inclusive+exclusive): 3-pass (block scan → scan block-totals → add offsets). The block
> scan is COALESCED (striped global I/O + blocked shared scan; Hillis-Steele cross-thread via `Select`+clamped-index+`Materialize`).
> Bit-exact **CPU oracle + Vulkan + DX12** (`[kir]`+`[gpu][kernel][scan]`). **Bench vs CUB `DeviceScan`: N=16.7M DRAM-bound 0.425 ms
> (630 GB/s ACTUAL = 94% peak) vs CUB 0.227 = 0.53×.** ⛔ HONEST — first primitive we DON'T crush; STRUCTURAL not kernel-slow: our
> kernels hit 94% peak but move 4N bytes vs CUB's SINGLE-PASS decoupled-lookback 2N (device atomics + forward-progress spin a
> portable deterministic scan can't use). Portable floor = 3N (2-pass reduce-then-scan ⇒ ~0.67×, the next lever); 2N parity needs
> CKIR atomics. ⛔ 2 scars fixed: (1) CKIR lazy shared re-read corrupts a read-then-write accumulator → freeze originals with
> Materialize [[FFT ping-pong]]; (2) `dispatch_kernel_1wg` harness MISSING TransferDst→ShaderRead barrier upload→dispatch = latent
> RACE only a FAST no-shared kernel exposed (read zeros) → fixed (hardens ALL harness users). kir 638/119, vulkan+dx12 [kernel] green, tidy clean.
> **⛔⛔ SCAN CRUSH — INVESTIGATED EXHAUSTIVELY, IMPOSSIBLE while bit-exact (2026-07-13, user "go for full crush, we need to crush scan"):**
> built the ATOMICS SUBSTRATE (`buffer_decl_coherent` = coherent-volatile / globallycoherent + new `KStmtKind::SpinUntilNonzero`,
> ALL 5 emitters + oracle; forward-progress + coherence PROVEN on Vulkan, no deadlock) + a single-pass chained scan (correct +
> bit-exact oracle+Vulkan). **FUNDAMENTAL FINDING: a bit-exact portable scan CANNOT crush CUB.** CUB's speed = decoupled
> look-back, which sums each block's prefix in a TIMING-DEPENDENT order (aggregates vs prefixes) ⇒ non-deterministic f32 rounding
> ⇒ NOT bit-exact (violates the ⭐⭐ mission). Both bit-exact single-pass forms measured SLOW: chained (waits for predecessor's
> full prefix) serializes → 0.03–0.08×; all-aggregate (fixed-order sum of ALL predecessor aggregates) O(nblocks²)+wave-serial →
> 0.015× + buggy. So scan is the ONE primitive where bit-exactness+portability STRUCTURALLY forbid a crush (forbids the whole
> fast-algorithm class, unlike FFT's no-FMA which still won DRAM-bound). **Verdict: portable bit-exact scan = 3-pass (0.53×) /
> 2-pass-3N (~0.67×). Atomics substrate KEPT (reusable for sort/histogram/compaction — those needn't be bit-exact).** Board +
> memory updated. kir 640/120, GPU correctness green, tidy clean.
> **▶ SORT (radix) STARTED — the bit-exact CRUSH path (user 2026-07-13 "preserve bit-exactness AND crush"):** a STABLE LSD radix
> sort IS bit-exact by construction (a permutation, stable ties via a deterministic per-block SERIAL rank — no atomic-race) AND
> memory-bound ⇒ CAN crush CUB while keeping the mission (unlike scan). New `ckir_sort.hpp` + a `SharedAtomicAdd` primitive
> (`atomicAdd(shared[bin],1)` — the COUNT is order-independent ⇒ bit-exact; all 5 emitters + oracle). **Increment 1 ✅: the
> HISTOGRAM kernel** (`build_sort_histogram`, 8-bit digit) — oracle bit-exact vs a direct per-block digit count (all 4 digits).
> kir 644/121, tidy clean. **Increment 2 ✅: OFFSET-SCAN + SCATTER + full 4-pass driver — the COMPLETE stable LSD radix sort
> works bit-exact on the oracle** (`build_sort_offsets`: per-bin block-prefix via a runtime `For` + Hillis-Steele of bin totals;
> `build_sort_scatter`: serial-thread-0 stable rank + scatter). Verified: histogram==direct count · offset==exact bin-major prefix
> (partitions [0,n)) · full 4-pass output fully sorted + XOR/sum-checksum permutation. ⛔ 2 scars: (1) `stmt_for_begin(count)`
> takes a VALUE NODE not a C++ int — passing the int made the loop count a garbage node ⇒ hang; (2) lazy shared re-read AGAIN —
> the scatter's `rk` fed both `s_cnt[d]=rk+1` AND `dest=off+rk`; the increment ran first so `dest` saw rk+1 ⇒ whole output
> shifted by 1 (gap at 0, last key dropped) → `stmt_materialize(rk)` before the increment. kir 649/122, tidy clean.
> **Increment 3 ✅: FULL SORT SYSTEM ON GPU — the complete 4-pass pipeline (histogram→offset→scatter ping-pong) DISPATCHES on
> Vulkan, output fully SORTED + valid permutation (30 assts); bit-exact on the oracle too. CUB gold (`cub_radixsort_bench.cu`):
> 16.7M u32 = 1.055 ms / 15.9 Gkeys/s / ~508 GB/s (76% peak).** ⭐ CRUSH is REAL: our 4-pass = ~8N traffic; at our 94% peak
> (630 GB/s) a MEMORY-BOUND scatter = ~0.85 ms for 16.7M = ~1.24× over CUB — needs the PARALLEL-rank scatter (current serial
> thread-0 rank is correct+stable but compute-bound). Lever: deterministic parallel rank = 8-bit local counting sort in shared
> as two 4-bit sub-sorts (`seg_hist[threads][16]`=16KB; CKIR lacks the warp ballot/match CUB's 8-bit rank uses). Board updated.
> **Increment 4 ✅: FULL bit-exact radix sort DONE (2-level parallel rank) + ⚠ EARLIER "18×" WAS A MEASUREMENT BUG (2026-07-13).**
> Built the 8-bit FULLY-PARALLEL deterministic rank = two stable 4-bit local counting sorts (transposed per-thread histogram
> `F[16·threads]` → block exclusive-scan → stable scatter; `build_sort_scatter`). Bit-exact (oracle 9/2 + vulkan 30/1 + dx12 green).
> **The prior "18× slower / 0.056×" report was WRONG — two artifacts:** (1) a 67 MB host→device staging copy INSIDE the timed
> region (~5 ms PCIe), and (2) a SILENT BUILD FAILURE — `getenv` in the diag tripped MSVC C4996 under /WX, so hours of "isolation"
> ran a STALE binary. Both fixed. FRESH-build breakdown (16.7 M, wall==gpu-ts): **empty/barriers 0.05 ms; histogram ≈0; scatter
> ≈5.7 ms; OFFSET ≈10 ms ← dominant; FULL 14.9 ms**. The offset (`build_sort_offsets`) is SINGLE-WORKGROUP looping nblocks=8192
> twice with global I/O — the fix is a parallel scan (bin-major hist ⇒ flat device exclusive-scan) → ~0.4 ms (>20× kernel win).
> **CEILING: bit-exact radix moves ~12 N (separate histogram pass) vs CUB onesweep's ~9 N (fused, decoupled-lookback = NON-bit-exact);
> CUB is at the 1.055 ms memory-bound peak. So sort is PARITY/LOSS-class (~1.2× memory-bound), NOT a crush — a FUNDAMENTAL wall like
> scan (bit-exactness forbids the fused fast algorithm).** Real crushes stand: FFT 1.99×/1.16×, reduction 1.42×, R2C 2×.
> **NEXT: (a) parallelize the offset (perf polish, ~5 ms competitive); (b) GEMM/MLP vs cuBLAS = the next real crush target (NRC moat).**

> **Increment 5 ◧: CKIR SUBGROUP OPS — the sort-crush unlock, core primitive DONE + bit-exact (2026-07-13).** User chose the
> subgroup path (over offset-polish / GEMM). Added `KOp::SubgroupBallot` (pred → u32 lane-mask) + `KOp::SubgroupBallotExclCount`
> (mask → popcount below this lane) + builders `subgroup_ballot`/`subgroup_ballot_excl_count`. Oracle models a FIXED 32-lane
> subgroup (loops lanes, evals pred per-lane — feasible because the oracle runs threads in lockstep). GLSL emitter →
> `subgroupBallot(...).x` / `subgroupBallotExclusiveBitCount(uvec4(...))` + `GL_KHR_shader_subgroup_ballot` extension; added to
> `is_fusable` + `pv` + `rhs`. Tests: `test_ckir_subgroup.cpp` (oracle) + a Vulkan test — **GPU == CPU oracle BIT-EXACT** (within-
> subgroup odd-rank). kir 650/123 + gpu-kernel 148/11 green, tidy clean. This is the CHEAP DETERMINISTIC RANK building block.
> **REMAINING for the sort crush:** compose into a grouping-INDEPENDENT block digit-rank (bit-exact across subgroup sizes) →
> rewrite `build_sort_scatter` to use it (memory-bound rank) → fused histogram (all 4 digits, one N-read pass = 9N) → parallel
> offset → then 0.96 ms = **1.1× crush**. Plus: `subgroupSizeControl` (force 32 on AMD/Intel) + the other 4 emitters (HLSL
> `WaveActiveBallot`/`WavePrefixCountBits`, CUDA `__ballot_sync`/`__popc`, MSL `simd_ballot`, WGSL `subgroupBallot`).

> **Increment 6 ✅: PARALLEL OFFSET — sort 14.9 ms → 6.26 ms (2.4×) (2026-07-13).** Replaced the single-workgroup serial offset
> (~10 ms) with TWO per-bin kernels (`build_sort_offset_local` + `build_sort_offset_gbase`, grid=nbins): local blocked-exclusive-scans
> each bin's column → within-bin prefix + grand total; gbase exclusive-scans the totals + adds each bin's base ⇒ FULL global_offset,
> bit-IDENTICAL to the serial kernel (oracle offset-verify still green). Fresh bench **6.26 ms / 2681 Mkeys/s vs CUB 1.055 = 0.17×**
> (was 0.07×). Offset now ~1.4 ms; the SCATTER 2-level rank (~5.7 ms, compute-bound) is now the bottleneck. kir sort 9/2 + vulkan
> sort 33/1 green. **NEXT: replace the scatter rank with the subgroup rank ([[increment 5]] primitive) = the memory-bound rank.**

> **Increment 8 ✅: SERIAL DEEP-PROFILE + 4 measured levers — sort 4.40 → 2.41 ms = 0.437× (2026-07-13, session 14.9→2.41 = 6.2×).**
> METHOD: skip-a-kernel diag is CONTAMINATED (stale data downstream) — built `[.sort-kprof]` standalone profiler (each kernel
> batch-timed alone on VALID precomputed inputs). TRUE baseline: hist .121/off_l .177/off_g .123/**scatter .814 (65%)**. Levers:
> (1) LOCAL REORDER scatter (CUB structure: register-stage key/digit/rank across rank rounds → reorder in shared → COALESCED
> per-digit run writes; dest bytes IDENTICAL) .814→.429; (2) variants epb4096/512-thr/tagged-2-barrier all MEASURED WORSE
> (occupancy/rounds — empirics>models; 512-thr epb4096 also blew the 48KB Vulkan shared cap = silent UB, exit 42);
> (3) GBASE FOLD: offset_gbase 8MB-rewrite → 1-WG totals-scan gb[256], scatter adds gb[d] (.123→.001); (4) BIN-MAJOR hist+off
> layout (hist writes bin·nblocks+blk, L2-coalesces across blocks; off_l column contiguous) off_l .177→.041. All bit-exact,
> oracle 9/2 + vulkan 33/1 + full 650/123 + 151/11 green, tidy clean. **Scatter now 72% (0.43 = 0.21 mem + 0.22 rank machinery).
> QUANTIFIED remaining path: (a) SubgroupMatch IR op → subgroupPartitionNV on NV (hw match_any; ballot-loop fallback elsewhere,
> same mask ⇒ bit-exact) ⇒ ~0.55×; (b) ONESWEEP — u32 lookback sums are ORDER-INDEPENDENT ⇒ bit-exact (f32 scan wall does NOT
> apply!); needs stmt_for_break_if IR (oracle: per-iteration active-set filter) ⇒ ~0.7-0.8×; both+tuning ⇒ parity band 0.9-1.0×.
> Board: docs/bench/2026-07-13-gpu-compute-primitives.md.**

> **Increment 9 ✅: BOTH final levers built + measured — ONESWEEP LANDS 2.20 ms = 0.48× CUB (2026-07-14; session 14.9→2.20 = 6.8×).**
> LEVER A `SubgroupMatch` (KOp + oracle lane-compare + GLSL subgroupPartitionNV + VK_NV_shader_subgroup_partitioned device enable):
> bit-exact but MEASURED SLOWER (scatter .43→.59 — 8 independent ballots pipeline on Ada; partition serializes) → reverted from the
> hot path, op kept in CKIR. LEVER B ONESWEEP: new IR `ForBreakIf` (per-thread For-break; oracle=per-iteration active-set filter;
> 5 emitters; GLSL/WGSL need bool() around the cond) + `BufferAtomicAdd` (5 emitters; HLSL byte-addressed .InterlockedAdd);
> kernels build_sort_ghist (fused 4-digit global hist, ONE N-read) / build_sort_clear / grid-indexed gbase (grid=4) /
> build_sort_scatter_onesweep (publish (cnt<<2|status) coherent → walk-back spin+add+BREAK-on-prefix → publish prefix → reorder →
> coalesced write). 7 dispatches/sort (was 13). **ONESWEEP 2.20 ms / 7440 Mkeys/s = 0.48× vs 4-kernel 2.61 = 0.40×.** All bit-exact
> (u32 sums order-independent; sorted+permutation pins keys-only output uniquely), suites 650/123 + 151/11 green, tidy clean.
> **HONEST: crush (>1×) NOT reached — remaining gap = the rank machinery (~0.22/pass) CUB hides in CUDA-only 99KB-shared 11K-key
> tiles + tuned SASS; Vulkan caps 48KB. Onesweep floor here ≈ 1.3-1.5 ms (0.7-0.8×) with a 2× cheaper rank (A/B paired rounds) or
> near-parity+ via a CUDA-backend bench of the SAME IR. Sort = PARITY-class portable; the real crushes stay FFT 1.99×/reduce 1.42×/R2C 2×.**

> **Increment 10 ◧: final micro-round — the ISSUE-BOUND frontier measured (2026-07-14, final 2.25 ms = 0.47×).** Three hypotheses
> measured on the onesweep scatter: (1) software-pipelined loads (pt hoisted above round barriers) = NO change ⇒ rank is
> instruction-issue-bound, not latency-bound (occupancy already hides DRAM); (2) branchless XOR match `bal^(bitv-1)` = ~1.5%
> (driver already optimized the select); (3) subgroupPartitionNV = slower (driver-emulated on consumer Ada). Both scatters carry
> the pipelined loads + XOR match; suites 650/123+151/11 green, tidy clean. **VERDICT: 8 ballots/key = info-theoretic minimum for
> ballot-based 256-way match; 4 barriers/round = minimum for the cross-subgroup stable scan; 48KB Vulkan shared forbids CUB's 11K
> tiles. The one credible >1× vector left: bench THIS same IR via the CUDA BACKEND (99KB shared, native intrinsics) — CUB's own
> turf, same bit-exact semantics. Then GEMM/MLP (NRC moat).**

> **Increment 11 ✅: CUDA-BACKEND CAMPAIGN — same IR on CUB's turf: 1.77 ms = 0.59× (2026-07-14; campaign 14.9→1.77 = 8.4×).**
> Emitted the onesweep via `emit_compute_kernel_cuda` ([.emit-cuda-sort] tool test → bench/gpu-compute/ckir_onesweep_gen.cu;
> driver ckir_onesweep_bench.cu = CKIR A/B/C + CUB in ONE binary). CUDA emitter gained SubgroupBallot/ExclCount/**Match**
> (__ballot_sync/__popc-lane/__match_any_sync) + volatile coherent params. THREE hard scars fixed (memory:
> [[cuda-subgroup-sync-divergence-and-lookback-scars]]): (1) lazy-inlined `popc(match(d))` inside the leader-if = full-mask
> *_sync with inactive lanes = DATA-DEPENDENT hang (sorted data ⇒ fewer leaders) — diagnosed by injecting device-printf into
> the GENERATED source; fix = stmt_materialize(mask) in uniform flow (both scatters); (2) blockIdx lookback DEADLOCK (launch
> order unguaranteed) → new IR `BufferTicket` (block-scoped atomicAdd ticket = virtual block id; 5 emitters + oracle);
> (3) __threadfence-per-spin = L2 livelock → volatile + __nanosleep. **RESULTS: A(2048)=1.775 ms/9453 Mk/s = 0.59×;
> B(4096)=1.82; C(8192, 41KB shared)=2.16 (32 barrier-rounds LOSE); CUB same-binary = 1.043.** CUDA beats Vulkan(2.20) by 24%.
> All sorted+permutation ✔; suites 650/123 + 151/11 green; all tidy clean. **NEXT (the specified parity/crush increment): the
> WARP-SYNCHRONOUS RANK — CUB ranks a tile with ~3 __syncthreads (per-warp counters + ONE block scan) vs our 32 (4×8 rounds);
> needs a warp-scoped statement tier (SyncWarp + warp-accumulate) in CKIR. Then GEMM/MLP.**

> **Increment 12 ***: WARP-SYNCHRONOUS RANK SHIPPED -- SyncWarp IR tier + CUB rank structure: CUDA 1.42 ms = 0.735x CUB
> (2026-07-14; campaign 14.9 -> 1.42 = 10.5x).** New IR `KStmtKind::SyncWarp` (CUDA __syncwarp / GLSL subgroupBarrier / MSL
> simdgroup_barrier; HLSL+WGSL conservative block barrier; oracle=commit). Onesweep scatter rank rebuilt: warp owns a
> CONTIGUOUS chunk (position order == (warp,round,lane) == rank order => STABLE), per-digit counters accumulated
> warp-synchronously (2 syncwarps/round, ZERO block barriers in the rank loop; was 4/round), ONE cross-warp scan => rank =
> wbase + within-warp rank. Block barriers/scatter 33 -> ~6. Vulkan 2.25->1.87 (0.56x); CUDA A=1.62 / **B(4096)=1.415 =
> 0.735x** / C=1.54 vs CUB 1.04 same-binary; all sorted+permutation. Regs 75-111, no spills. Per-config clear grids; digr
> register array dropped. suites 650/123 + 151/11 green, tidy clean. **Remaining 1.36x, itemized: ghist ~0.12 (privatize /
> dual-stream overlap with clear), scatter ~0.08/pass over the 0.21 memory floor (reorder bank conflicts -> padding;
> 384-thread config). STRUCTURE is now at CUB parity (onesweep+match+warp-rank) portably in CKIR. Then GEMM/MLP.**

> **Increment 13 (FINAL) ✅: inch-grind closed the sort campaign at 1.42 ms = 0.73× (2026-07-14).** Per-kernel CUDA profile:
> ghist 0.109 = N-read FLOOR (already optimal — predicted privatization/overlap wins didn't exist); clear 0.012; gbase 0.004.
> Bank-conflict padding MEASURED WORSE (1.464 vs 1.418; digit-scatter ~conflict-free) → reverted. Remaining ~0.11/pass =
> warp-sync round-chain latency (double-key rounds / ncu = diminishing). **SORT CLOSED: 14.9→1.42 = 10.5×, bit-exact, portable;
> scoreboard: FFT+reduce+R2C = CRUSH ✅; scan = f32 wall; sort = structural parity 0.73×. All suites green, tidy clean.**

> **Increment 14 ✅: THE NRC MOAT — fused MLP vs cuBLAS, forward + backward, DOUBLE CRUSH (2026-07-14).** Fully-fused 64-wide
> MLP (6 layers, batch 1M, fp16/fp16-acc), tensor cores (wmma), activations NEVER leave the chip — the tiny-cuda-nn / Neural
> Radiance Cache technique cuBLAS structurally cannot express (fusion across GEMM calls is off its menu). **Forward 2.37×**
> (cuBLAS 2.68 ms → fused 1.13 ms, 45.6 TF, bit-exact rel=0); **backward 1.90×** (cuBLAS 9.60 ms at its BEST split-K algo →
> fused 5.06 ms, 20.4 TF, rel=5e-5 vs fp32 CPU oracle); **full training step 1.98×**. Standalone gold refs:
> `bench/gpu-compute/mlp_fused_bench.cu` + `mlp_backward_bench.cu`; board `docs/bench/2026-07-14-fused-mlp-cublas-gold.md`.
> ⚠ FAIRNESS: the first backward showed 8.14× — a STRAWMAN (cuBLAS default algo ran the huge-K=1M dW GEMM at 37 ms);
> `cublasLtMatmulAlgoGetHeuristic`+256MB workspace picks split-K → 2.64 ms (14×), real crush = 1.90×. Levers: forward win =
> 4 row-fragment ILP (16 wmma chains/warp); backward dW = a^T·dz reduced over batch (fp32 atomic, per-warp `sdw` scratch —
> block-shared raced → rel 0.54 bug); NOATOMIC probe = dW scatter is 35% (200M atomics, count-bound & L2-resident; split-K
> spread to DRAM was WORSE). NGROUP=8 marginal best.

> **Increment 15 ✅: CKIR PORT increment 1 — CUDA FORWARD authored in CKIR (2026-07-14).** New `engine/kir/include/crd/kir/
> ckir_mlp.hpp` (monolithic per-backend recipe emitter, same pattern as the coopmat2 GEMM tensor tier): one `MlpConfig` →
> `emit_fused_mlp_fwd_cuda` emits the wmma forward kernel + `mlp_forward_ref` CPU oracle. Test `tests/kir/test_ckir_mlp.cpp`
> (oracle == hand-computed 2-layer, bit-identical; emit well-formedness) + `[.emit-cuda-mlp]` writes
> `bench/gpu-compute/ckir_mlp_fwd_gen.cu`; driver `ckir_mlp_bench.cu` compiles via nvcc + duels cuBLAS. **CKIR-authored kernel
> = 2.41× crush** (cuBLAS 2.83 → 1.17 ms), `max_abs_diff=0` vs cuBLAS, **0/67M halves differ run-to-run = BIT-IDENTICAL** (the
> {1..16} determinism pillar, forward). Architecture note: cooperative-matrix ops do NOT map to CKIR's per-invocation
> statement tier (like the coopmat2 GEMM), so tensor kernels are whole-kernel recipe emitters, not KStmt-built. crd-kir 706/125
> green on MSVC + clang-cl; both new files tidy-clean.

> **Increment 16 ✅: CKIR PORT increment 2 — Vulkan coopmat2 forward → PORTABLE across the two primary compute backends
> (2026-07-14).** `emit_fused_mlp_fwd_glsl` (ckir_mlp.hpp) emits a `VK_NV_cooperative_matrix2` workgroup-scoped kernel: one
> workgroup owns a batch tile, activations ping in SHARED across all layers (the fusion — never touch global between layers),
> the fp32 accumulator round-trips a shared scratch for ReLU/linear + fp16 repack. Test `tests/kir-vulkan/test_backend_vulkan.cpp`
> `[mlp]`: emit → shaderc → dispatch through the unified VulkanComputeContext → gate vs the CPU oracle. **rel = 0.0021 (fp16
> tol), det_diff = 0/524288 (BIT-IDENTICAL run-to-run).** Same MlpConfig → CUDA wmma + Vulkan coopmat2, both matching one
> oracle, both deterministic = the mission's "portable" made real for the forward. ⚠ FIXED a latent Inc-15 bug: the CPU oracle
> read weights COLUMN-major (`w[k+W·n]`) — a transpose of the kernel's ROW-major (`w[k·W+n]`); a host convention-probe vs the
> live CUDA kernel proved row-major (col-major was 95× wrong). Oracle is now the true shared reference. crd-kir 706/125 +
> crd-kir-vulkan GPU 33027/32 green on MSVC + clang-cl; tidy-clean. **NEXT: (a) fan forward out to HLSL WaveMatrix / MSL
> simdgroup_matrix / WGSL subgroup-matrix; (b) BACKWARD pass in CKIR with a fixed-order deterministic dW reduction (no atomics).**
> Board `docs/bench/2026-07-14-fused-mlp-cublas-gold.md`.

> **Increment 17 ✅: CKIR PORT increment 3 — forward on ALL backends via the FP32-PRECISE tier, bit-exact (2026-07-14).**
> ⚠ The named tensor primitives are PHANTOM on this toolchain (measured): HLSL WaveMatrix REJECTED by DXC 1.8.2502 (never
> shipped; DX12 compiles cs_6_0), WGSL subgroup-matrix absent from wgpu-native, MSL simdgroup_matrix has no Metal on Windows.
> Emitting them = theater. User chose FP32-precise (AskUserQuestion) = the STRONGER moat (bit-exact across ALL backends,
> which the fp16 tensor tier cannot be). `build_mlp_fwd_fp32` (ckir_mlp.hpp) builds the fused MLP on the CKIR STATEMENT TIER
> (1 workgroup=1 sample, 1 thread=1 output feature, activations ping-pong in shared = the fusion; ascending precise fold =
> no FMA); ONE builder → the GENERIC per-backend emitters lower it to all 5. Verified: CPU oracle bit-exact (512 ==);
> **Vulkan RUN bit-exact; DX12 RUN bit-exact & IDENTICAL to Vulkan** (the {1..16} cross-backend moat on the MLP); CUDA
> nvcc-compiles; WGSL+MSL emit-validated (no imperative WebGPU ctx / no Metal here). Tests: test_ckir_mlp.cpp (FP32 CPU +
> all-5-emit + [.emit-cuda-mlp-fp32]), test_vulkan_context.cpp [mlp], test_dx12_compute.cpp [mlp]. crd-kir 1234/127 +
> gpu-context vulkan/dx12 [mlp] green; tidy-clean. Tensor tier (CRUSH) stays CUDA wmma + Vulkan coopmat2; FP32 tier is the
> PORTABLE+BIT-EXACT companion.

> **Increment 18 ✅: CKIR PORT increment 4 — the BACKWARD pass with a DETERMINISTIC dW reduction (2026-07-14).** Two
> statement-tier builders in ckir_mlp.hpp, NO atomics: `build_mlp_bwd_dz` (per-sample activation-gradient chain — 1 wg = 1
> sample, dz[l] on-chip via ReLU′ `select(a[l+1]>0,g,0)`, da=dz·Wᵀ→next g, writes dz_all) + `build_mlp_bwd_dw` (the
> DETERMINISTIC weight-gradient reduction: dW[l][k][n] = Σ_r a[l][r][k]·dz[l][r][n] in ASCENDING sample order = fixed order,
> no atomics). The standalone's fp32-atomicAdd dW was non-deterministic; this is bit-exact AND run-to-run bit-identical.
> Verified: CPU oracle `mlp_backward_ref` bit-exact (28k ==); **Vulkan dz+dW bit-exact (bad==0) + dW run-to-run BIT-IDENTICAL
> (det==0)**. Both on the statement tier ⇒ generic emitters lower to all backends. Tests: test_ckir_mlp.cpp (backward CPU) +
> test_vulkan_context.cpp [mlp]. crd-kir 28883/128 green MSVC + clang-cl; tidy-clean. **NRC-moat-in-CKIR STATUS: forward =
> tensor crush (CUDA wmma 2.41× + Vulkan coopmat2) + FP32-precise portable/bit-exact (Vulkan+DX12 run, CUDA nvcc, WGSL/MSL
> emit); backward = FP32-precise portable/bit-exact + deterministic dW (Vulkan run). The full NRC moat is now expressed in
> CKIR.**

> **B14-c-1 ✅: SVGF edge-stopping À-TROUS denoiser — the first RESUMED-DETOUR frontier slice (2026-07-14).** Resumed D-007
> at the next in-order ⬜ (#20 B14, real-time GI). B14-a ReSTIR needs ray tracing (B9/C3, later); user chose (AskUserQuestion)
> the RT-independent **B14-c denoiser**. `engine/kir/include/crd/kir/ckir_svgf.hpp` `build_svgf_atrous` = a STATEMENT-TIER
> compute pass (gathers its own 5×5 stencil from storage buffers — not a deferred renderer leaf like the B13 resolve passes):
> increasing-stride à-trous blur with depth/normal/luminance edge-stopping weights, variance-guided. Verified: CPU-oracle
> INVARIANTS (uniform preserved <1e-5 · noisy variance drops >2× · hard depth edge STOPS the bleed) + **Vulkan AND DX12 ==
> oracle at maxrel 3.58e-7** (arithmetic bit-exact, exp/pow ULP — the B8 transcendental bar). **⛔ EMITTER GAP FIXED: the
> compute-kernel value emitter LACKED Exp/Pow (the fragment/material path had them) — the mirror of the "raster lags compute"
> scar; added to all 5 backends (glsl/hlsl/cuda/msl/wgsl).** Tests: test_ckir_svgf.cpp (3 invariants) + test_vulkan_context.cpp
> + test_dx12_compute.cpp [svgf]. crd-kir 28886/131 + gpu-context vulkan/dx12 [svgf] green MSVC + clang-cl; tidy-clean.

> **B14-c GOLD-STANDARD SVGF DENOISER ✅ (2026-07-15, user: "full crush and gold standard system and full quality CKIR").**
> Upgraded to the FAITHFUL Schied 2017 formula, built the temporal half + the full pipeline — the complete SVGF-2017 gold
> denoiser in CKIR. **B14-c-1 GOLD à-trous**: depth-GRADIENT edge weight (∇z from unit-stride neighbours — edges hold under
> oblique surfaces, vs a flat |Δz|) + 3×3-GAUSSIAN-blurred variance for the luminance φ (kills firefly survival) + variance
> filtered as Σ(hw)²Var/(Σhw)² (Schied §4.2). **B14-c-2 TEMPORAL** (`build_svgf_temporal`): reproject-along-motion (nearest) +
> disocclusion reject (depth/normal/off-screen → α=1 ⇒ reprojected sample discarded, no ghost) + EMA blend α=max(1/hist,α_min)
> + luminance moments m1,m2 → Var=m2−m1². **B14-c-3 PIPELINE**: temporal → à-trous ×5 (steps 1,2,4,8,16). Verified: à-trous
> invariants (uniform preserved · noise smoothed · silhouette stops) + temporal (24-frame accumulation halves error · history
> grows past 8 · disocclusion resets) + FULL PIPELINE crushes noise **≥10×** vs the raw frame end-to-end; **Vulkan à-trous
> 3.64e-7 + temporal BIT-EXACT 0.0 (no transcendentals), DX12 à-trous 3.64e-7.** crd-kir 28894/133 + vulkan/dx12 [svgf] green
> MSVC + clang-cl; tidy-clean.

> **B14-c COMPLETE ✅ (2026-07-15): B14-c-4 A-SVGF adaptive-α — the full gold SVGF/A-SVGF denoiser is in CKIR.** Added the
> A-SVGF temporal-gradient adaptive α to `build_svgf_temporal` (config `asvgf`): where the incoming sample is a real OUTLIER
> from the history (|Δl| ≫ σ_hist = √Var_hist, not noise), boost α toward 1 ⇒ accumulation RESETS ⇒ no lag/ghosting on fast
> lighting/motion change; noise (|Δl| < asvgf_lo·σ) does NOT reset. Variance-aware (Schied 2018 spirit), self-contained (reuses
> the moments the temporal pass already tracks — no separate gradient/stratification buffer). Verified: fixed-α AND adaptive
> both keep a long history through the noisy stable phase (noise doesn't reset), but on a real step change **adaptive tracks the
> new value >5× closer** (kills the lag). Vulkan bit-exact (2.98e-8, sqrt-ULP). crd-kir 28897/134 + vulkan/dx12 [svgf] green
> MSVC + clang-cl; tidy-clean. **B14-c (spatiotemporal denoiser) = DONE: gold à-trous + temporal + pipeline + A-SVGF.** NEXT in
> B14: [B14-b] world-space radiance cache (DDGI probes — the no-RT-HW GI tier) and/or [B14-a] ReSTIR when ray tracing (B9/C3)
> lands. Optional B14-c polish: ReBLUR/ReLAX/SIGMA diffuse/specular/shadow separation + firefly clamp.

> **B14-b DDGI (dynamic diffuse GI) COMPLETE ✅ (2026-07-15, "same gold standard approach").** The no-RT-HW GI tier — Majercik
> 2019 visibility-moment irradiance probes, gold-standard, in CKIR (`engine/kir/include/crd/kir/ckir_ddgi.hpp`, statement-tier
> compute; RT ray-generation is the deferred B9 leaf, the analytic core is built+verified). **[B14-b-1] octahedral** encode/
> decode (Cigolle 2014 — whole sphere → [−1,1]², no singularity; round-trip recovers EVERY dir) + **Chebyshev visibility**
> (depth-moment σ²/(σ²+Δ²) occlusion, no light leak). **[B14-b-2] probe SAMPLE** (`build_ddgi_sample`): 8-probe trilinear ×
> normal-wrap × Chebyshev × octahedral irradiance-in-normal-dir → leak-free indirect diffuse; verified uniform-field→exact-C +
> occlusion cuts a leaked probe ≥4×. **[B14-b-3] probe UPDATE** (`build_ddgi_probe_update`): integrate rays (cosine irradiance
> + cos^sharpness depth moments) + temporal hysteresis; verified aligned-rays accumulate + depth mean → hit distance, back-faces
> stay dark. Verified CPU (55 asserts) + Vulkan (sample 1.32e-7, update 1.82e-6). crd-kir 28952/139 + vulkan [ddgi] green MSVC +
> clang-cl; tidy-clean. **✅ POLISHED (2026-07-15): bilinear octahedral sampling (4-tap) + full ARBITRARY probe grid (cell =
> floor((pos−origin)/spacing) clamped to [0,grid−2], 8 corner probes via flat index; 4×4×4 indexing verified — a bright probe
> lights only its own cell). CPU 57 asserts + Vulkan re-verified 1.31e-7. B14 GI now has BOTH a denoiser (B14-c) AND an
> indirect-light source (B14-b).**

> **B14-a ReSTIR ESTIMATOR COMPLETE ✅ (2026-07-15, "maximum gold standard arsenal, no gaps").** The dominant real-time
> many-light/GI sampler (Bitterli 2020), authored in CKIR (`engine/kir/include/crd/kir/ckir_restir.hpp`, statement-tier
> compute; the candidate GENERATION — which light, its shaded contribution, the shadow-ray visibility — is the deferred B9 RT
> leaf, the reservoir/RIS/reuse ESTIMATOR is built+verified, same pattern as SVGF/DDGI). A reservoir = [f(y), p̂(y), W, M].
> **`build_restir_ris`**: Weighted-Reservoir-Sampling streams M candidates, keeps i w.p. w_i/Σw, W = Σw/(M·p̂(y)), estimate =
> f(y)·W. **`build_restir_temporal`**: merges the current + reprojected-previous reservoir, each weighted by its own Σw = p̂·W·M,
> with prev M clamped to m_cap·M_cur (bounds temporal bias). Verified CPU oracle — RIS is **UNBIASED** (pixel-mean of f(y)·W ==
> the true light integral even under an imperfect p̂=1 constant target) + temporal reuse **DROPS VARIANCE ≥2×** (canonical p̂=f
> target ⇒ the merged estimate is the running mean over the ACCUMULATED candidate stream, var ~ Var(f)/M_eff; still unbiased; M
> grows). Vulkan portability: RIS 1.11e-7, temporal 1.19e-7 == oracle (add/mul/div/min/max/cmp/select only ⇒ bit-exact, div the
> lone IEEE-ULP). crd-kir 28959/142 + vulkan [restir] green MSVC + clang-cl; tidy-clean. **NEXT: B15 — physically-based sky +
> atmosphere (Hillaire 2020: transmittance LUT + multiple-scattering + sky-view LUT + aerial perspective), gold standard.**

> **B15-a-1 atmosphere TRANSMITTANCE LUT (2026-07-15, "maximum gold standard arsenal").** Started B15 (sky/atmosphere), the
> next visual-frontier slice. Hillaire 2020 / UE5 Sky-Atmosphere, in CKIR (`engine/kir/include/crd/kir/ckir_atmosphere.hpp`,
> `crd::kir::atmos`, statement-tier compute — pure analytic scattering integrals, no RT leaf, so the whole thing is buildable +
> bit-exact-verifiable now). `build_atmos_transmittance`: one thread per LUT texel over the Bruneton (r,mu) parameterisation,
> ray-marches (40 steps) the extinction integral tau = integral of sigma_e ds with sigma_e = beta_R*exp(-h/H_R) +
> beta_M_ext*exp(-h/H_M) + beta_O*tent(h) [Rayleigh + Mie + ozone], stores T = exp(-tau) per RGB channel. The Bruneton mu mapping
> was reformulated CANCELLATION-FREE (H^2-rho^2 = H^2(1-v^2) - never a difference of km-scale squares, whose f32 ulp ~= 4 would
> wreck the long horizon march). Verified CPU physics: T in (0,1]; the Rayleigh **blue>green>red** extinction ordering
> (T_red~=0.94, T_blue~=0.76 straight up - why the sky is blue and sunsets red); horizon path far darker; T->1 at altitude.
> Vulkan **maxabs 6.25e-5** - the correct metric for a [0,1] LUT (a relative metric divides by near-zero horizon T); this is the
> hardware exp/sqrt-vs-libm floor over a 40-step exp accumulation (all add/sub/mul/div bit-exact), NOT a logic gap.

> **B15-a-2/3 atmosphere MULTISCATTER + SKY-VIEW LUTs (2026-07-15).** Extended `ckir_atmosphere.hpp` with the rest of the core
> Hillaire pipeline. **`build_atmos_multiscatter`** (B15-a-2) — the isotropic 2nd-order fill light: per texel (μ_sun,alt),
> integrate single scattering over a Fibonacci sphere of directions (each a short march that bilinear-samples the transmittance
> LUT for sun-transmittance), form L₂ and the transfer f_ms, then sum the closed-form series Ψ = L₂/(1−f_ms). CPU physics: Ψ≥0
> and finite, grows toward the ground, bluish (Rayleigh-dominated), brighter with the sun up; Vulkan 1.46e-5. **`build_atmos_
> skyview`** (B15-a-3, the renderable sky) — march the view ray adding single scattering (Rayleigh + Cornette-Shanks Mie phase,
> weighted by sun-transmittance from the transmittance LUT) + isotropic multiple scattering (from the multiscatter LUT),
> attenuated by the view transmittance. CPU physics: a blue zenith, a Mie glow toward the sun, a bright horizon; Vulkan 1.43e-5.
> Hot shared nodes (r_x, μ_sun_x, the bilinear coords) are `stmt_materialize`d — register reuse on GPU AND it collapses the
> recursive CPU-oracle's exponential re-walk (the oracle has no memo cache; it re-evaluates every shared subtree). Portability
> metric is ABSOLUTE (a [0,1]/small-radiance LUT); ~1e-5 = the hardware exp/sqrt-vs-libm floor, all add/sub/mul/div bit-exact.
> CPU-oracle tests run at reduced LUT resolution (the interpreter is O(texels·steps·nodes); physics + portability are per-texel
> identical at any size — production uses the full 256×64 / 192×108 defaults). tidy-clean.

> **B15-a-4 aerial-perspective froxels — B15-a COMPLETE (2026-07-15).** `build_atmos_aerial` — a 3D camera-frustum volume: one
> thread per (x,y) froxel column marches through the depth slices, writing each cell's running (RGB inscatter + mean
> transmittance) via the same single+multiple-scatter integrand; composites into B12-d fog as `surface·T + inscatter`. CPU
> physics: transmittance falls with depth, inscatter accumulates, bluish (Rayleigh); Vulkan 1.79e-5. **▶ B15-a is the full
> Hillaire atmosphere: transmittance + multiscatter + sky-view + aerial, all CKIR, all CPU-physics + Vulkan verified.**

> **B14-a ReSTIR COMPLETE + GI/atmosphere PERFORMANCE board (2026-07-15, user: "finish B14 no gaps, full crushing perf").**
> (1) **ReSTIR SPATIAL reuse** `build_restir_spatial` — the other half of spatiotemporal reuse: generalises the 2-reservoir
> temporal merge to K+1 (center + K screen neighbours), streaming WRS with one random per neighbour, merged W = Σ(all Σw)/(Σ all
> M·p̂). CPU: unbiased + variance ≥2× lower (K=4 ⇒ 5 reservoirs' candidates); Vulkan 1.29e-7. **▶ B14-a ReSTIR = RIS + temporal
> + spatial, complete.** So B14-a/b/c are all COMPLETE (ReSTIR + DDGI + SVGF); the only remaining B14 item is **B14-d NRC**, the
> NEURAL tier whose crushing perf tier explicitly rides **B10 coop-vectors** (its FP32 MLP already crushes cuBLAS — the NRC
> functional core = hash-grid encode + MLP inference/train is a large next slice). (2) **GPU PERF board** (`[.gi-bench]`, Vulkan
> `last_gpu_ms` min-of-30, RTX 4070 Ti SUPER → `docs/bench/2026-07-15-gi-atmosphere-vulkan.md`): DDGI probe-sample **0.171 ms**
> /1080p (12.2 Gqueries/s, ~65% peak BW), SVGF à-trous **0.236 ms**/1080p, ReSTIR RIS **0.527 ms**/262 k px, and the ENTIRE
> Hillaire sky (4 LUTs) **~0.12 ms/frame**. The portable bit-exact IR runs in real time — zero portability tax. crd-kir
> **30524/147** + vulkan **850/84** green; tidy-clean; bench board banked. **NEXT: B14-d NRC functional core; then B15-b clouds.**

> **HONEST CRUSH — CKIR-emitted vs HAND-WRITTEN GLSL (2026-07-15, user: "honest crush as always, THEN B14-d NRC").**
> `[.crush-bench]`: same algorithm, same buffers, GPU-timed, OUTPUT bit-matched (honesty gate). First measurement was a LOSS —
> `build_restir_ris` **1.56× SLOWER** than hand-written (0.486 vs 0.312 ms) but BIT-IDENTICAL output ⇒ the loss is pure code
> STRUCTURE: the statement-tier builders UNROLLED the M=32 candidate loop (32× straight-line + hoisted index temps → register
> pressure → occupancy collapse on a memory-bound kernel). **SOLVED**: rewrote to a tight RUNTIME loop (`stmt_for_begin` +
> per-thread SHARED reservoir accumulators) → **0.318 ms = PARITY** with the hand register loop (1.02×), crushes the old unroll
> 1.53×, still bit-exact (Vulkan 1.11e-7). Two emitter hazards solved: shared-RMW re-read double-counts Σw (→ `stmt_materialize(Σw)`)
> and a cross-scope temp `p` (used inside the loop AND at the post-loop output store → undefined ⇒ `stmt_materialize(p)` hoists
> it). Transmittance (COMPUTE-bound) kept as UNROLL — its shared-loop was 1.47× WORSE; residual 1.23× vs a hand REGISTER loop is
> the compute-emitter's lack of register-carried loop values (scoped gap; a negligible once/frame LUT). **⭐ RULE: memory/
> occupancy-bound big loops → runtime SHARED loop, not unroll; compute-bound → unroll.** Boards:
> `docs/bench/2026-07-15-ckir-vs-handwritten-glsl.md`. crd-kir 30524/147 + vulkan 850/84 green; tidy-clean.

> **▶▶ B14-d NRC FUNCTIONAL CORE COMPLETE → ALL OF B14 DONE (2026-07-15, user: "finish B14-d, close B14").** The neural
> radiance cache in CKIR (`engine/kir/include/crd/kir/ckir_nrc.hpp`, `crd::kir::nrc`, statement-tier; all Vulkan **bit-exact vs
> oracle = 0.0**): **B14-d-1 `build_nrc_hashgrid_encode`** — the Instant-NGP MULTIRESOLUTION HASH-GRID encoder (L levels ×
> trilinear-blended hashed features; verified zero-table→0, trilinear PARTITION-OF-UNITY, determinism); **B14-d-2
> `build_nrc_infer`** — the cache query (encoded → ReLU hidden → RGB, == a hand-computed forward); **B14-d-3
> `build_nrc_train_grad`** — the online-step BACKPROP (L2-loss gradient → dW1/dW2, verified == FINITE DIFFERENCES). The
> coopmat/fused MLP PERF tier rides B10 (the FP32 MLP already crushes cuBLAS). **⛔ EMITTER BUG found + fixed: the statement-tier
> compute Const emitter mangled a u32 constant > INT_MAX** (MSVC `static_cast<int>(2654435761)` → INT_MIN); the hash was the
> first statement-tier kernel to use such a seed. Fixed to `app_int_const`/`%lld` across ALL 5 backends (glsl/hlsl/cuda/msl/wgsl);
> golden byte-exact emissions held (small consts unchanged). **▶▶ B14 is COMPLETE — a/b/c/d: ReSTIR + DDGI + SVGF + NRC, the
> full real-time GI stack in CKIR.** crd-kir **30531/150** + vulkan **864/86** green; tidy-clean.

> **▶▶ B15-b VOLUMETRIC CLOUDS COMPLETE → B15 DONE (2026-07-15, "finish B15 full frontier, full crush/portability/zero bugs").**
> Nubis (Schneider) in CKIR (`ckir_clouds.hpp`, both Vulkan bit-exact): **`build_cloud_density`** — the industry-standard
> **PERLIN-WORLEY** shape (Perlin-FBM DILATED by inverted Worley-FBM = the cauliflower cumulus) × height gradient × coverage carve
> × high-freq **WORLEY** erosion; **fully procedural + portable**, density∈[0,1], vanishes at slab edges, grows with coverage;
> Vulkan 1.51e-7 (`optimize=true`) + **`build_cloud_march`** (BAKE density→3D VOLUME, march SAMPLES it trilinearly + BEER-POWDER +
> light-march-to-sun + Cornette-Shanks phase + multi-octave MS; Vulkan 4.77e-7). **⛔⛔ SIX general CKIR portability wins found+
> fixed:** (1) recursive CPU oracle had NO memo → deep noise graphs intractable → generation-keyed MEMOIZATION (guarded OFF for
> subgroup ops) — full kir suite 200s→29s; (2) statement-tier Const emitter mangled u32 const > INT_MAX (`static_cast<int>`→
> INT_MIN) → `app_int_const`/`%lld` all 5 backends; (3) `BitOr/And/Xor` of BOOL operands (`gradient3`'s `(h==12)|(h==14)`) emitted
> illegal `bool|bool` → emit `||`/`&&`/`!=`; (4) UNROLLED 27-cell Worley blows shaderc+driver super-linearly → **`worley3_loop`
> emits a RUNTIME LOOP over the 27 cells** (running-min order-INDEPENDENT ⇒ bit-exact; shared min accumulator) ⇒ gold Perlin-Worley
> compiles in seconds; (5) the compute-kernel GLSL emitter's statement path is **SCALAR-ONLY** (vec3/dot/swizzle are raster-only) ⇒
> compute noise must SCALARIZE; (6) a value shared across sibling For-bodies (or a top-level value stranded inside a loop) must be
> `stmt_materialize`d at top level (else GLSL "undeclared identifier"); (7) a cross-backend emit gate then exposed that the
> **CUDA/MSL/WGSL** compute emitters INLINE-EXPANDED values (no CSE, unlike GLSL/HLSL's `temped[]`) ⇒ the deep perlin+hash DAG blew
> up EXPONENTIALLY (128 MB OOM) → gave all three the same node-id `decl`/`matd` materialization pass ⇒ all 5 backends emit the gold
> cloud density compactly (debt `ckir-offhost-emitter-cse` FIXED same session; existing off-host structural tests unchanged). **▶▶
> B15 COMPLETE — atmosphere + clouds, gold Perlin-Worley fully procedural, portable across ALL 5 backends.** crd-kir **30554/153** +
> vulkan **874/88** green; tidy-clean.

> **▶ B16 STARTED — WATER/OCEAN + CAUSTICS, full frontier 2026 (2026-07-15).** Audit first (user "full frontier"): the B8
> **frontier technique stack** table now carries an explicit STATUS column — EVERY row's shader math is built + bit-exact both
> backends (VSM/EVSM/MSM/PCSS/LTC/split-sum/Forward+/DQS all present; corrected a mis-audit — VSM *is* built); the only deferrals
> are device/render-pipeline plumbing (uniform, post-detour) + B2-e sampler-feedback (DX12-only, non-portable). Then **[✅ B16-a-0]
> transcendental compute substrate** — the ocean spectrum needs `log`/`tanh`/`atan2` (Box-Muller Gaussian · dispersion depth · wave
> direction); the compute-kernel emitters only had `sqrt/sin/cos/exp/pow/floor` (the full set lived only in the raster emitter). Wired
> `Log/Log2/Tanh/Atan2/Atan/Asin/Acos/Sinh/Cosh` into ALL 5 compute emitters (oracle's `apply_unary/binary` already had them); a
> 5-backend emit gate + CPU-oracle test + **Vulkan dispatch = oracle to ULP (4.77e-7)** all green. crd-kir **30569/154**; tidy-clean.
> **[✅ B16-a-1] ocean SPECTRUM** (`ckir_ocean.hpp` `build_ocean_spectrum`) — Horvath directional spectrum (DigiPro 2015: JONSWAP +
> Mitsuyasu/Hasselmann spread + swell) + gravity-capillary finite-depth dispersion → h₀(k), pre-packing [h₀(k), conj(h₀(−k))] per
> texel. CPU-oracle physics test green. **[✅ B16-a-2 (2026-07-15)] time-evolution + batched 2-D IFFT + assemble — the FULL FFT-ocean
> update, portable + GPU-validated.** New `build_ocean_evolve` (h̃(k,t)=h₀e^{iωt}+conj(h₀(−k))e^{−iωt}, then Tessendorf's packing of
> 8 real fields — height/displacement Dx,Dz/slope/Jacobian-gradients — into 4 EXACTLY-Hermitian complex spectra for ONE batched
> IFFT) → `build_fft2d_c2c_batched` (NEW reusable 2-pass transpose-on-write STRIDED batched inverse 2-D FFT; extended
> `build_fft1d_radix16`'s strided path with the per-image batch offset) → `build_ocean_assemble` (displacement map + shading normal +
> Jacobian foam coverage). Verified: CPU-oracle end-to-end vs a direct inverse-DFT reference (foam∈[0,1], unit normals); batched IFFT
> **bit-exact on Vulkan AND DX12** (re/im-bad 0); full pipeline (evolve→IFFT→assemble) **dispatches on Vulkan, maxrel 1.01e-6 vs oracle**.
> kir 4130-assert ocean+fft2d green; all touched files tidy-clean. **⚠ HONEST BENCH (`docs/bench/2026-07-15-fft-ocean-batched-ifft.md`):
> the BARE batched IFFT is at the campaign's no-FMA wall — parity/loss in the L2-resident regime (0.35–0.94× cuFFT; 48 MB L2 ⇒ n≤512·B≤16
> stays resident), edging cuFFT only at the 512²·B=64 DRAM spill (1.03×). A plain IFFT has NO fusion lever ⇒ NOT a crush.**
> **⭐ [✅ FUSION CRUSH — the SOLVE, built + measured] `build_ocean_evolve_rowfft`** folds the time-evolution INTO the row-IFFT's first
> global load (computes h̃(k,t) + the field-f packed value inline, then the radix-4 row IFFT) ⇒ the ocean update collapses 4 dispatches →
> 3, deleting the evolve dispatch + the whole packed-spectrum global round-trip. **Bit-exact vs the un-fused pipeline** (shared
> `detail::dispersion`/`evolve_pack` helpers + identical radix-4 stages; CPU-oracle bad==0, and on **Vulkan** fused res == un-fused res
> BIT-EXACT). Measured (`[.ocean-fused-bench]`, last_gpu_ms min-of-30): **N=256 1.13× · N=1024 1.10×** over the un-fused ocean — the
> fewer-global-round-trips win a cuFFT-based ocean CANNOT match (cuFFT must run a separate evolve kernel writing the spectrum we delete).
> Board updated: `docs/bench/2026-07-15-fft-ocean-batched-ifft.md`. (⚠ radix-4 fused; a radix-16 fused variant for the largest N + multi-
> cascade batch>4 ride a-3.)
> **⭐ [✅ 5-BACKEND EMIT GATE] the ocean kernels (spectrum/evolve/fused-rowfft/assemble) now EMIT on ALL FIVE compute backends**
> (`test_ckir_kernel_emit.cpp` `[ocean]`: GLSL/HLSL/CUDA/MSL/WGSL, structural markers — tanh/barrier/select) — the "fully CKIR, fanned
> out everywhere" proof (Vulkan+DX12 already RUN bit-exact/ULP; CUDA/MSL/WGSL golden-string like the rest of the campaign).
> **▶ B16-a-3 STARTED (2026-07-15): the multi-scale ocean.** `OceanCascadeConfig` (C patches of different L, per-cascade λ) +
> multi-cascade pipeline: a-2 run per cascade into **ONE batched 4·C-field IFFT** (the DRAM-bound regime the batched/fused crush
> targets — 3 cascades = batch-12, CPU-verified each cascade vs a direct DFT reference + cascades distinct). `build_ocean_foam_accumulate`
> — TEMPORAL foam (whitecaps persist ×decay, re-inject at Jacobian pinch: foam(t)=max(foam(t−1)·decay, inject)); CPU-verified.
> crd-kir 4167-assert green; tidy-clean.
> **[✅ a-3 GPU-VALIDATED] foam-accumulate BIT-EXACT on Vulkan + the multi-cascade batched IFFT at batch=4C (4/12/16) BIT-EXACT on
> Vulkan+DX12.** ⚠ this exposed + FIXED a latent RACE in the shared `dispatch_fft2d` harness (missing `TransferDst→ShaderRead`
> upload barrier → flaky/all-wrong once the grid > device occupancy, batch≥9; kernel was always correct — `[.ocean-ifft-bench]`
> self-verified at batch 64). Root-fixed, batch 8–16 now bit-exact both backends; SANITY ledger + [[feedback_dispatch_1wg_missing_upload_barrier_race]].
> **[✅ batch-4C crush bench]** the bare batched IFFT crushes only in the DRAM-bound regime (n=512·batch=64=128 MB ⇒ 1.03× as cuFFT
> triples across its L2 spill); typical cascade counts (C=3–4) stay L2-resident ⇒ no-FMA wall, so the FUSION (1.10–1.13×) is the
> regime-independent crush. Board updated.
> **▶ B16-a-4 STARTED (2026-07-15): water shading.** New `ckir_water.hpp` (`crd::kir::water`) — **[✅ a-4-1 core] `water_shade`**:
> the frontier water surface BRDF — air-water Schlick Fresnel DIELECTRIC split (sky env + GGX sun glitter on the reflected side vs
> Beer-Lambert depth-absorbed refraction on the transmitted side) + subsurface backscatter (backlit-wave glow) + foam overlay, reusing
> the B8 `lighting::d_ggx`/`f_schlick_scalar`. **Bit-exact vs a line-by-line f64 reference** on the CPU oracle (`[water]`; the graph
> runs F64 to isolate op structure — the B8 methodology). ⭐ ARCHITECTURE NOTE: water shading is FRAGMENT-stage (vec3 dot/swizzle), so
> it fans out via the **raster** path (GLSL/HLSL/MSL/WGSL — the fragment-capable backends), NOT the 5-compute-backend gate the ocean
> SIMULATION uses (the compute-kernel emitters are scalar-only + CUDA has no fragment stage). ⛔ debugging scar: `eval_cpu` shape must be
> `make_shape({kN})` not `{1}` (a `{1}` graph processes one element ⇒ the uninitialised output tail reads garbage, masquerading as a
> math blow-up). crd-kir green; tidy-clean.
> **[✅ a-4-2 beauty pieces] `ocean_sun_glitter`** — the SIGNATURE broad sun path: a GGX lobe whose roughness comes from the wave-SLOPE
> VARIANCE σ² (α=√(2σ²), Beckmann-slope↔GGX) so the highlight is a wide statistical sparkle (far/rough water broadens it), not a plastic
> point; reuses the B8 microfacet core (d_ggx·v_smith·F·NoL). **`sky_color`** — analytic horizon→zenith gradient + a soft exp sun disk+halo
> for the reflected ray (until the reflected ray samples the B15 Hillaire sky / SSR). Both **bit-exact vs f64 references** (`[water]` 3 cases).
> **⭐ [✅ a-4-1 RASTER OBSERVABLE — IT RENDERS] `water_shade` DRAWS on Vulkan AND DX12, PIXEL-IDENTICAL.** Shared `build_water_fs`
> (`ckir_raster_triangle.hpp`) — a fullscreen water quad, normal tilting with FragCoord so the Fresnel + sun glint sweep, Reinhard-
> tonemapped — through the create_program seam: **centre RGB (6,29,29) deep teal body + a bright white sun glint (maxlum 645), byte-for-byte
> the same on both backends** (`[raster][ocean]`). This is the fragment fan-out proof (shading is fragment-stage ⇒ raster backends, per the
> a-4-1 architecture note) AND the "see it render" milestone — the water is real on real GPUs. tidy-clean.
> **NEXT (finish a-4): (1)** the world-uv multi-cascade COMBINE (B2 texture sample of the C cascade maps) + fold `ocean_sun_glitter`/`sky_color`
> into the master shader (displaced/tessellated water mesh consuming the a-3 displacement/normal/foam maps); **(2)** screen-space SSR reflection
> + underwater god-rays (Arc Blanc JCGT 2025). Then **B16-b caustics**. Deferred to B16-close: full 4-config sweep.
> **[✅ DISPLACED-GEOMETRY OCEAN, 2026-07-16] — the fragment normal-map showed the FFT's full spectrum as busy "white noise"; the
> smooth directional swells (gasgiant/AC4 refs) need real displaced GEOMETRY.** New CKIR op **`SampleIndexedLod`** (bindless ARRAY
> sample at an explicit LOD — the combo CKIR lacked, since a VS has no derivatives; appended at END of `KOp`, cook-stable; builder
> `tex_sample_at_lod` with lod in the ext pool; GLSL+HLSL emitters + nonuniform gate; MSL/WGSL graceful-defer = same status as the
> existing bindless SampleIndexed). Then the **Johanson PROJECTED-GRID** water: `build_ocean_displaced_vs` (screen-space lattice from
> `VertexIndex`, raycast onto the water plane with the EXACT FS camera, SampleIndexedLod-displaced by the swell height with a distance
> LOD ramp + far-taper for a clean horizon, projected back with the inverse-camera closed form — **no mat4**) + `build_ocean_water_geo_fs`
> (shades from the interpolated world pos; the high-freq CHOP is a **per-pixel mip-filtered normal map** in the FS — has derivatives ⇒
> NO aliasing, the "white noise" fix — the AAA split: geometry=swell, FS-normal=chop). Rendered `[.ocean-frame]` → **`ocean_geo_*.bmp`**:
> real 3-D swell silhouettes, crisp detail, calm directional rollers, teal SSS, intermittent foam, clean hazy horizon — matches/beats the
> AC4 reference. Composited over the fragment sky by coverage-α (no new context load-op). Built+ran win-release (66 asserts green).
> **[✅ 4-CASCADE + JOINT-JACOBIAN FOAM, 2026-07-16 — user art-direction]** production 4-spectrum ocean (`OceanCascadeRender`): 4
> non-harmonic world scales (LCM tiling ⇒ non-repeating); BIG cascades carry high-amplitude rolling-swell GEOMETRY, FINE cascades are
> low-amplitude per-pixel normal detail. Each bakes `[nx,nz,h,½(1−J)]`; the FS SUMS the per-cascade folds → the JOINT Jacobian foam
> (folding of the combined surface — first-order exact) ⇒ generous whitecaps across the crests. Teal-green body + stronger sun-glitter
> path. Widened the projected-grid over-fetch (no corner gaps). Matches reference #2 (a Sea-of-Thieves-class confused sea). 87 asserts
> green.
> **[✅ B4 MESH-SHADER CORE — GPU-PROVEN 2026-07-16]** the modern amplification path runs end-to-end in CKIR: a `Mesh` KEntry
> (`mesh_vertices`/`mesh_primitives`/`mesh_prim`, per-vertex position+out from the workgroup builtins; entry_valid checks) →
> `emit_mesh_glsl` (GL_EXT_mesh_shader: SetMeshOutputsEXT + guarded gl_MeshVerticesEXT[tid]/gl_PrimitiveTriangleIndicesEXT[tid]) →
> shaderc `shaderc_mesh_shader` @ SPIR-V 1.4 → device: **VK_EXT_mesh_shader + meshShader + maintenance4** (gated so the non-mesh
> device is byte-identical), a mesh shader object (**NO_TASK_SHADER flag** — the fix for an else-silent device-lost) via
> `create_mesh_program`, **vkCmdDrawMeshTasksEXT** via `draw_mesh`. The `[mesh]` triangle proof RENDERS (red-center/blue-corner,
> pixel-identical to vertex-pull). ⭐ scar: raster Vec2/Vec3 emitter hardcoded `vec3(` → a uvec3 emitted `vec3(...)` (compile fail);
> fixed to `vtype(nd.type)` (no-op for float ⇒ canary held). Full raster suite 614 asserts green + ocean 87 green (no regression).
> **[✅ OCEAN MESHLETS RENDER 2026-07-16]** the 4-cascade ocean renders as MESHLETS pixel-identical to vertex-pull:
> `ocean_projected_vertex` factored as the ONE shared raycast/displace/project; `build_ocean_displaced_mesh` (8×8-vertex patches,
> 32×32=1024 meshlets, WorkgroupIndex→patch + LocalInvocationIndex→local vert/prim) + `draw_mesh_bindless_depth`. `ocean_mesh_*.bmp`
> == `ocean_geo_*.bmp`. ⭐ 3 device scars: (1) glslang caps mesh local_size at 128 → 8×8 patch (98 threads); (2) VUID-08690 — with
> meshShader enabled EVERY vkCmdDraw must bind MESH=null (else device-lost) → set_draw_state does it; (3) **nonuniformEXT on a
> CONSTANT bindless index returns ZERO in a mesh shader (NVIDIA)** → emitter omits it for constant indices (what made the mesh
> displacement finally read the FFT). Raster suite 614 green, ocean 102 green.
> **[✅ B16 VISUAL POLISH 2026-07-16 — user spec]** the ocean now looks awesome (matches ref_ocean_2.png): (1) TEMPORAL foam
> restored (22-step warmup, foam=max(prev·decay,inject), decay 0.965 — accumulates then fades exponentially, never instant) +
> TEXTURED in the FS (Worley bubbles + fractal break-up, not flat gray); (2) GENTLE waves + amplitude split (low-freq tall geometry,
> high-freq normal-map); (3) SEAMLESS horizon (grazing veil → output ALPHA = transparency to the real sky, dissolves the seam);
> (4) AA SSAA 3×; (5) god rays tuned (visible shafts, no white-out); (6) DARKER defined clouds (coverage 0.66 + density self-shadow).
> [[project_ocean_visual_gaps_before_b16_close]]. Ocean 105 asserts green. **NEXT: B16-close DoD (tidy + 4-config sweep), then carry
> on D-007 (B17 OIT → …). Minor remaining: faint horizon line, softer sun, more directional god-ray shafts.**
> **[✅ HLSL MESH EMITTER 2026-07-16]** `emit_mesh_hlsl` (DX12/SM-6.5: `[outputtopology]`+`[numthreads]`+`SetMeshOutputCounts`+`out
> vertices`/`out indices`, SV_Position LAST); DXC `ms_6_5`/`as_6_5` wired; the `[hlsl][mesh]` gate compiles the triangle + the real
> bindless ocean meshlet HLSL → SPIR-V via DXC. **Mesh portability: Vulkan RENDERS · DX12 emit+DXC-validated · WGSL/MSL = vertex-pull
> fallback** (WebGPU has no mesh; caller guards on `mesh_shader()`). Raster 620 green. **▶ #1 mesh-shader fast path DONE (Vulkan render
> + DX12 emit).** **NEXT: #2 PROMOTE the ocean out of the harness into a reusable node-editor-drivable CKIR pass (+ the WGSL-portable
> texture_2d_array form of the bindless-LOD sampling); then B16-close DoD (tidy + 4-config sweep).**
> **[✅ #2 PROMOTE 2026-07-16]** the displaced-geometry ocean render pass is lifted out of the test harness into the ENGINE —
> new `engine/kir/include/crd/kir/ckir_water_render.hpp` (`crd::kir::water`): `OceanCascadeRender` config + `ocean_grid` camera +
> the shared `ocean_projected_vertex` + `build_ocean_displaced_vs` (vertex-pull) + `build_ocean_displaced_mesh` (meshlets) +
> `build_ocean_water_geo_fs` (surface). The renderer + node editor can now drive it; the tests alias it into `crd::gputest` (call
> sites unchanged). Ocean 102 green, raster 620 green (identical render). **▶ #1 + #2 + #3 all DONE.** **NEXT: B16 VISUAL POLISH
> (user-flagged 2026-07-16, must-fix for gold — [[project_ocean_visual_gaps_before_b16_close]]: horizon-seam outline, streaky foam,
> softer sun, wispy clouds vs ref_ocean_2.png), then B16-close DoD (tidy + 4-config sweep), then carry on D-007 (B17 OIT → …).**

> **Increment 19 ✅: CKIR PORT increment 5 — DX12 backward + CUDA TENSOR backward (2026-07-14).** (a) The FP32-precise
> statement-tier backward now runs on DX12 too — bad_dz==0, bad_dw==0, BIT-IDENTICAL to Vulkan + oracle (deterministic dW
> portable across both runnable statement-tier backends). (b) `emit_fused_mlp_bwd_cuda` (ckir_mlp.hpp) — the CKIR tensor
> backward recipe (reduce_dw + fused wmma: dz mask + on-chip da wmma chain + dW=aᵀ·dz wmma reduced over batch → fp32 atomic
> into NGROUP partials); generated `ckir_mlp_bwd_gen.cu` nvcc-compiles + `ckir_mlp_bwd_bench.cu` **reproduces the crush 1.94×
> vs cuBLAS** (best split-K), dW rel=5e-5. ⚠ fp32-atomic dW = crush tier (per-backend, not bit-exact); FP32 statement-tier
> backward is the deterministic companion. Vulkan coopmat2 backward NOT built (coopmat2 batch-reduction dW disproportionately
> hard; Vulkan has the deterministic FP32 backward). Tests: test_ckir_mlp.cpp (backward CPU + [.emit-cuda-mlp-bwd]) +
> test_dx12_compute.cpp [mlp]. crd-kir 28883/128 green MSVC + clang-cl; tidy-clean. **⭐ THE FULL NRC MOAT IS NOW IN CKIR:
> forward inference crush (CUDA 2.41× + Vulkan coopmat2) + backward training crush (CUDA 1.94×) + deterministic bit-exact
> training (Vulkan + DX12). NRC-moat-in-CKIR campaign COMPLETE.** This is the B10 (neural / cooperative-vectors) compute
> substrate for the detour, built ahead + doubling as the hesap-GPU ML substrate.


> **Increment 7 ✅: SUBGROUP-RANK SCATTER + privatized histogram — sort 6.26 → 4.40 ms (2026-07-13).** Replaced the 2-level local
> sort with a SUBGROUP-BALLOT rank (`build_sort_scatter`): pt rounds (strided), per key MATCH same-digit lanes via radix_bits
> ballots → within-subgroup rank (exclusive bit-count) + leader writes the subgroup count to seg[sg][d]; cross-subgroup exclusive
> scan folds a per-round dhist accumulator ⇒ block-rank = # same-digit-before, dest = global_offset[d]+rank. NO local sort, NO bh
> (scatter back to 3 buffers), shared 43→17 KB. Bit-exact oracle 9/2 + vulkan 33/1 (needed: `sg` materialized out of the leader-if
> scope [[feedback_ckir_if_block_shared_temp_scope_materialize]]; `~0u` not the 0xFFFFFFFF const; BitNot/BitCount added to GLSL
> pv/rhs — latent bug). Histogram privatized to K=threads/32 per-warp sub-histograms (contention cut). **Fresh bench 4.40 ms /
> 3809 Mkeys/s vs CUB 1.055 = 0.24×** (from 0.07× at session start — 3.4× total). **HONEST CEILING: the load is now DISTRIBUTED
> (hist/offset/scatter each ~1–2 ms, murky — skip-diag is CONTAMINATED because skipping a kernel feeds stale data to the others);
> privatization only saved 0.13 ms. The portable bit-exact sort plateaus ~0.24× CUB — CUB's onesweep uses hardware `match_any` +
> tuned atomics + decoupled-lookback (non-bit-exact) we can't portably match. Same wall as scan/the DRAM-bound doctrine: sort's
> compute/atomic/sync phases don't hide the portability tax.** All machinery (subgroup ops, parallel offset, subgroup rank, priv
> histogram) DONE + bit-exact. Remaining levers (bin-major/coalesced offset, histogram fusion) give diminishing returns, not a crush.
> (Orientation + research + IR + oracle + ALL 5 EMITTERS + both-GPU dispatch + For/If + 1D FFT + fused-1D-conv + **2D FFT + fused-2D-conv (256² crush + batched DRAM-bound 1.16× CRUSH)** DONE; B-hdr complete.)
> After B-cmp FFT: B14 (GI/ReSTIR), B15–B19, B4, C3/B9 (RT), C6/B10 (neural), C5/B11 (work graphs), D1–D5, then EXIT → v17. **⭐ PLAN EXPANDED (user 2026-07-12): added B12 (screen-space
> lighting — GTAO/AO · Hi-Z SSR · SSGI · volumetric fog+god-rays · screen-space SSS) + B13 (post — TAA · BLOOM/lens for
> neon/LED/sci-fi glow · auto-exposure+AgX/ACES tonemap+LUT · DoF+motion-blur · CA/vignette/grain/CAS/FSR) AFTER B8, before
> mesh/RT — the raster visual completion; RT counterparts land with B9.** **⭐⭐ PLAN GREATLY EXPANDED via deep 2026-SOTA research (user 2026-07-12,
> "fully frontier cutting-edge 2026 state, fill all gaps"): 5 research agents surveyed the frontier + papers → added
> B14 (GI+denoise: ReSTIR + world-space radiance cache + SVGF/ReBLUR denoiser + own-NRC via CKIR autodiff) · B15 (Hillaire
> sky + Nubis clouds) · B16 (FFT ocean + caustics) · B17 (OIT: WBOIT/MBOIT/A-buffer) · B18 (hair: Marschner/Chiang/dual-scatter)
> · B19 (3D Gaussian Splatting) · B-cmp (the hesap-GPU compute primitives — FFT/sort/scan/reduce/GEMM — built DURING the detour).
> Upgraded B12 (AO→visibility-bitmask SSILVB · SSR→hybrid+radiance-cache · phase-function family) + B13 (ML-upscaler seam+frame-gen
> · FFT-bloom · AgX/Tony/PBR-Neutral/ACES2 tonemap decoupled from HDR-output · geometric specular AA). Extended B4 (visibility
> buffer + SW rasterizer, Nanite-class) + B8-i (RT/MegaLights/deep shadows) + B8-l (clustered decals). See D-007 master table.**
> Locked order: full visual frontier (raster → materials/lighting/B8 → **B12 → B13 → B14 GI → B15 sky/clouds → B16 water → B17 OIT
> → B18 hair → B19 splatting → B-cmp** → B4 mesh → B9+C3 RT → B10 neural → B11+C5 work-graphs → Phase D cook) → THEN hesap-GPU.
> **▶ B3-c ✅ COMPLETE (2026-07-11) — GLSL VS+FS emitters behind the seam.** (1) Hoisted the ~60-case value switch out of
> `emit_vec_glsl` into a shared `emit_value_stmt(g,i,s,leaf)` (+`emit_stmt_prefix`) — the `leaf` supplies the stage-specific
> RHS (compute `Input`→buffer; raster `StageIn`/`Builtin`→stage input, UBO `FieldGet`→`ubo.member`). **Compute byte-exact:
> kir-vulkan 33010 unchanged.** (2) New `emit_stage_glsl` VS/FS — prologue (`layout(location)` in/out · `layout(set,binding,std140)`
> UBO · builtins via `glsl_vsfs_builtin_name`) → main (shared emitter + `For` scaffold) → epilogue (`gl_Position` · colour
> attachments · `gl_FragDepth`). (3) `create_program(KGraph,KEntry)` routes Vertex/Fragment → `entry_valid` → `emit_stage_glsl`
> → `compile_glsl_to_spirv(stage)` → wrap. (4) **Gate green — gpu-context-vulkan 66/10:** a KIR VS + FS entry each compile to
> a valid SPIR-V program through the seam (correct stage, real SPIR-V); a vertex with no clip position is refused. 4 files
> tidy-clean.
> **▶ B3-d ✅ COMPLETE (2026-07-11) — HLSL VS+FS mirror.** Extracted the HLSL value switch → shared `emit_value_stmt_hlsl`
> (+`emit_stmt_prefix_hlsl`) — **compute byte-exact: kir-dx12 30821 unchanged.** New `emit_stage_hlsl` emits STRUCT-based
> VS/FS I/O: `[[vk::location(N)]]` pins the SPIR-V location, `SV_Position`/`SV_VertexID`/`SV_IsFrontFace` builtins via
> `hlsl_vsfs_builtin`, `cbuffer register(bN,spaceN)` UBOs, `SV_Target`/`SV_Depth` outputs. **Gate green — gpu-context-vulkan
> 73/11:** a KIR VS + FS entry each emit HLSL that DXC lowers to valid SPIR-V (with a dxc-absent soft-skip); a vertex with no
> clip position is refused. The DX12 gpu-context is compute-only, so B3-d gates via `compile_hlsl_to_spirv`; the DX12 raster
> *program* seam lands with C4. 2 files tidy-clean.
> **▶ C4 ✅ COMPLETE (2026-07-11) — DX12 `IRasterContext` (the D3D12 mirror of C1).** **C4-a:** `create_dx12_raster_context`
> — a graphics (DIRECT) queue + committed RGBA8 render targets + a clear-with-readback; the texture→READBACK copy honours the
> 256-byte `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` row pitch (100×64 target → 512B padded pitch, exercised). **C4-b:** a new
> `Dx12GpuContext : IGpuContext` (owns a device for adapter identity) that mints `Dx12GpuProgram`s — cooked DXIL via
> `make_dx12_program`, OR CKIR→HLSL→DXIL via `emit_stage_hlsl` + a public `compile_hlsl_to_dxil` (dxc, `vs_6_0`/`ps_6_0`,
> entry `main`, no `-spirv` ⇒ signed DXIL) — **the real DX12 create-program seam C4 promised**; plus a graphics-PSO
> `create_raster_program` (empty root sig, attributeless, cull-none, RGBA8 RTV) + `DrawInstanced`. DXIL is device-independent
> bytecode, so the standalone raster context builds the PSO from the program's blob — no shared device needed.
> **Gate green (real D3D12, MSVC + clang-cl — gpu-context-dx12 3/3):** clear+readback exact across all channels incl. the
> padded row-pitch · a KIR VS + FS entry each → DXIL program (a no-clip-position vertex refused) · a red triangle over a blue
> clear reads back centre=red / corner=blue. `[[vk::location]]` in the emitted HLSL is accepted by DXC for DXIL (concern
> unfounded). 5 files tidy-clean (`dx12_raster_context.hpp/.cpp` · `dx12_context.hpp/.cpp` · `test_dx12_raster.cpp`).
> **▶ B3-e ✅ COMPLETE (2026-07-11) — the IR-authored draw, ONE graph on BOTH backends.** A SHARED, backend-neutral CKIR
> triangle (`tests/gpu-shared/ckir_raster_triangle.hpp`) — a vertex entry deriving 3 clip positions from `VertexIndex` via
> a select-on-index chain (no dynamic array indexing → SROA-safe) + a constant-red fragment entry — is built ONCE and drawn
> through `create_program(KGraph)` → `create_raster_program` → `draw` → readback on BOTH Vulkan (GLSL→SPIR-V→shader
> objects) and DX12 (HLSL→DXIL→graphics PSO). Both read centre=red / corner=blue. This is the literal proof that ONE IR
> lowers to every backend (ADR-0101). **⭐ Surfaced + SOLVED a latent emitter bug:** the shared `emit_value_stmt`
> (`ckir_glsl.hpp`) / `emit_value_stmt_hlsl` (`ckir_hlsl.hpp`) emitted an INTEGER `Const` with a **float** literal
> (`int t = 0.0;`) — **type-strict GLSL rejects it, but HLSL coerces, so DX12 passed and only Vulkan failed** (same graph!).
> Both emitters now emit int literals for int/uint consts (matching the compute switch that already did). Compute
> byte-exact intact: **kir-vulkan 33010 · kir-dx12 30821 unchanged**; raster suites gpu-context-vulkan **12/83** ·
> gpu-context-dx12 **5/51**, green on MSVC + clang-cl. 5 files tidy-clean.
> **▶ B1 STARTED (fragment-stage foundation) — B1-a ✅ COMPLETE (2026-07-11): fragment derivatives.** New fragment-only KOps
> `DFdx`/`DFdy`/`Fwidth` (+ `dfdx`/`dfdy`/`fwidth` builders) — `is_fragment_only_op` + an `entry_valid` gate refuse them in
> any non-fragment stage; CPU oracle = 0 (single invocation has no 2×2 quad); GLSL `dFdx`/`dFdy`/`fwidth`, HLSL `ddx`/`ddy`/
> `fwidth` (raster emitters only — compute `default:` refuses). A shared `build_derivative_fs` (in `ckir_raster_triangle.hpp`)
> outputs `(dFdx(FragCoord.x), dFdy(FragCoord.x), 0, 1)` — since screen x rises exactly 1/pixel, `dFdx==1`/`dFdy==0` on every
> pixel and backend. **Gate green (real GPUs, MSVC + clang-cl): centre reads R≈255/G≈0 on Vulkan + DX12; compute byte-exact
> kir-vulkan 33010 · kir-dx12 30821 unchanged; 6 files tidy-clean.**
> **▶ B1-b ✅ COMPLETE (2026-07-11): fragment `discard` / alpha-test.** New `KEntry::discard_cond` (a BOOL node; <0 = none);
> `entry_valid` refuses it outside a fragment stage and requires a bool. Both raster emitters push it as a reachability root
> and emit `if (t_cond) { discard; }` before the colour writes. Shared `build_discard_fs` paints red but discards where
> `FragCoord.x < 16`. **Gate green (real GPUs, MSVC + clang-cl): the left half of the covered triangle is discarded (blue
> clear shows), the right half stays red, on Vulkan + DX12; compute byte-exact kir-vulkan 33010 · kir-dx12 30821 unchanged;
> 6 files tidy-clean.**
> **▶ B1-c ✅ COMPLETE (2026-07-11) — interpolation qualifiers, ALL FOUR observable on BOTH backends.** `Interp`
> {Smooth·Flat·NoPerspective·Centroid·Sample} on `stage_in`/`KStageOutput` (carried in `dset`); `entry_valid` forces an
> integer interpolant to `flat` on BOTH the VS-out and FS-in sides; GLSL `flat/noperspective/centroid/sample` + HLSL
> `nointerpolation/noperspective/centroid/sample` emitters. **flat** = int payload=200 readback · **noperspective** =
> perspective triangle (w={1,4,1}) where screen-linear (≈0.225) diverges from perspective-correct (≈0.069) at centre ·
> **centroid** + **sample** exercised on NEW **MSAA render targets** (`create_color_target_ms`, appended pure virtual):
> centroid diverges from centre-sample at edge pixels; a `sample`-qualified step over a full-screen tri antialiases the
> threshold (32 grey pixels) vs a hard 0/255 for `smooth`. Vulkan: 4× MSAA image + AVERAGE resolve attachment; DX12: MSAA
> texture + `ResolveSubresource` + a per-sample-count graphics PSO (the count is baked into a D3D12 PSO, so `Dx12RasterProgram`
> now owns the DXIL and builds/caches a PSO per count). **⭐ Three real bugs found + fixed** (all latent, all masked by NVIDIA
> leniency until now): (1) the HLSL emitter put `SV_Position` FIRST in `VSOut`, stealing output register 0 so EVERY VS→PS
> interpolant mismatched the PS input register → `E_INVALIDARG` PSO link on DX12 (would have broken all future
> material/lighting shaders); fixed by emitting it LAST (verified via DXIL signature dumps). (2) the `sample` qualifier emits
> the `SampleRateShading` SPIR-V capability but the context never enabled the `sampleRateShading` **device feature** →
> validation error; (3) `discard` (B1-b) emits `DemoteToHelperInvocation` but `shaderDemoteToHelperInvocation` was not
> enabled. Both features now enabled for graphics-capable contexts (compute device unchanged). **Gate green (real GPUs,
> MSVC+clang-cl+ASan+shipping): gpu-context-vulkan 134/18 · gpu-context-dx12 97/11 · Vulkan validation-layer SILENT across
> all 18 · compute byte-exact kir-vulkan 33010 · kir-dx12 30821 unchanged · C2 device-adoption + rhi-vulkan intact · 8 files
> tidy-clean.**
> **▶ B1-d ✅ COMPLETE (2026-07-11) — frag-depth write + early-Z flag + conservative depth.** New `KEntry::early_fragment_tests`
> (bool) + `DepthMode`{Any·Greater·Less}; `entry_valid` gates both to fragment, forbids `early_fragment_tests` + a `frag_depth`
> write (contradictory), and requires a `frag_depth` write for a conservative `depth_mode`. Emitters: GLSL
> `layout(early_fragment_tests) in;` + `layout(depth_greater/less) out float gl_FragDepth;`; HLSL `[earlydepthstencil]` +
> `SV_Depth`/`SV_DepthGreaterEqual`/`SV_DepthLessEqual`. **NEW depth-buffer infra both backends** (`create_color_depth_target`
> + `draw_depth(clear_depth, DepthCompare, …)` — append-only virtuals): Vulkan D32 depth image + dynamic depth-test state +
> dynamic-rendering depth attachment; DX12 D32 depth texture + DSV + a depth-enabled PSO (depth compare BAKED into the PSO,
> so `Dx12RasterProgram`'s PSO cache now keys on (samples, depth-func)). **Observable frag-depth**: a fullscreen tri at
> primitive depth 0 + a FS writing `gl_FragDepth = FragCoord.x/32`, depth cleared to 0.5 / LessEqual ⇒ the left half passes
> (red), the right FAILS (clear) — a split that appears ONLY because the shader wrote depth. **⭐ Fixed a real DXIL rule**:
> outputting `SV_DepthGreaterEqual`/`LessEqual` while reading `SV_Position` requires the position input to be
> `noperspective centroid` — the HLSL emitter now adds it for conservative-depth fragment shaders (found via the DXC
> validation error). early_fragment_tests + conservative depth are compile/validate-level (a behavioural early-Z test needs
> B1-f storage-buffer side effects). **Gate green (real GPUs, MSVC+clang-cl+ASan+shipping): gpu-context-vulkan 157/21 ·
> gpu-context-dx12 117/14 · Vulkan validation SILENT on the depth path · compute byte-exact kir 33010/30821 unchanged · 9
> files tidy-clean.** ⚠ **SCAR:** the `KEntry` layout change + ninja's missed header-mtime (Edit doesn't bump mtime) left the
> SHIPPING build with a STALE TU reading `KEntry` at old offsets ⇒ null programs / a config-specific failure that LOOKED like
> an LTCG miscompile; the fix was to DELETE the objects and clean-rebuild (touch didn't suffice).
> **▶ B1-e ✅ COMPLETE (2026-07-11) — VARIABLE-RATE SHADING, all three rate sources, both backends.** `KEntry::shading_rate`
> (a VS-output int node; `entry_valid` gates it to a position-writing stage) → GLSL `#extension GL_EXT_fragment_shading_rate`
> + `gl_PrimitiveShadingRateEXT` / HLSL `uint : SV_ShadingRate` (the per-PRIMITIVE "out"; packed `(Yshift<<2)|Xshift`, 2×2 = 5,
> identical on both APIs). Device infra: Vulkan enables `VK_KHR_fragment_shading_rate` + pipeline/primitive/attachment
> features (graphics-capable only) + queries the tile size; DX12 queries `D3D12_OPTIONS6` VariableShadingRateTier +
> `ID3D12GraphicsCommandList5`. Interface (append-only): `ShadingRate`/`ShadingRateCombiner` enums, `draw_vrs(pipeline_rate,
> primitive_combiner)`, `create_color_vrs_target(tile_rate)` (a per-tile R8_UINT rate image — Vulkan clears it, DX12
> uploads it), `supports_vrs()`. ⚠ **Shader-object gotcha:** enabling VRS makes `vkCmdSetFragmentShadingRateKHR` MANDATORY
> per draw ⇒ `set_draw_state` now sets a 1×1 default; `draw_vrs` overrides. **Observable via 2×2 BLOCKINESS**: a ramp FS
> (R = FragCoord.x/32) makes each 2×2 block share ONE invocation ⇒ horizontal even-x neighbours become equal (n=512) vs
> distinct at 1×1 (n=0). All three sources verified on BOTH backends: per-draw (0→512), per-primitive (512), attachment
> (512). Two bugs found: `gl_PrimitiveShadingRateEXT` is GLSL `int` (authored the rate as I32, not U32); the DX12 tier enum
> is `D3D12_VARIABLE_SHADING_RATE_TIER`, not `D3D12_SHADING_RATE_TIER`. **Gate green (real GPUs, MSVC+clang-cl+ASan+shipping):
> gpu-context-vulkan 177/24 · gpu-context-dx12 130/17 · Vulkan validation SILENT on the whole VRS path · compute byte-exact
> kir 33010/30821 unchanged · 11 files tidy-clean.**
> **▶ B1-f ✅ COMPLETE (2026-07-11) — conservative raster + inner coverage + fragment interlock/ROV + FS storage buffers, both
> backends. ▶ B1 CLOSED.** Four new capabilities behind append-only interface additions (`ConservativeMode` enum;
> `supports_conservative_raster/inner_coverage/fragment_interlock`; `draw_conservative(mode)`; `IStorageBuffer` +
> `create_storage_buffer`/`draw_storage`). **IR:** `KOp::StorageLoad` (appended at END) + `g.storage_load(idx)`;
> `KEntry{storage_write_index, storage_write_value, interlock}`; `KBuiltin::InnerCoverage` (appended at END; `entry_valid`
> gates uint index/value + fragment-only interlock). **Emitters:** GLSL `coherent buffer` SSBO + `GL_ARB_fragment_shader_interlock`
> `layout(pixel_interlock_ordered) in;` + begin/endInvocationInterlockARB + `gl_FragFullyCoveredNV`; HLSL
> `RasterizerOrderedStructuredBuffer`/`RWStructuredBuffer` at u0 + `SV_InnerCoverage`. **Device:** Vulkan enables
> `VK_EXT_conservative_rasterization`+EDS3 (conservative-mode + extra-overestimation-size dyn state) + `VK_EXT_fragment_shader_interlock`
> (pixel-ordered) + `fragmentStoresAndAtomics`; DX12 queries `D3D12_OPTIONS` ConservativeRasterizationTier + ROVsSupported.
> **Binding parity:** every raster program carries the FS storage slot (Vulkan set-0 SSBO descriptor / DX12 u0 UAV descriptor
> table) — unaccessed by non-storage draws; the DX12 root sig went from empty → 1 UAV table. Conservative is a PSO-baked
> state on DX12 (cache-keyed) vs an EDS3 dynamic state on Vulkan. **Observables (IDENTICAL on both backends):** conservative
> OVERESTIMATE covers 242→312 red pixels (the edge rim); inner coverage splits interior (220 white, ic=1) from edge rim (92
> black, ic=0) under overestimate; interlock RMW counter = EXACTLY 2 at the overlap of two triangles, 0 at background —
> deterministic. ⚠ **Validation caught 4 REAL bugs silent with layers off** (the scar): 3 Vulkan VUIDs — overestimate+shader-objects
> also needs `vkCmdSetExtraPrimitiveOverestimationSizeEXT` (07632), an FS storage write needs `fragmentStoresAndAtomics`
> (NonWritable-06340) — and 1 DX12 debug-layer ERROR — an FS reading `SV_InnerCoverage` links ONLY into a conservative PSO,
> so the program carries a `wants_conservative_raster()` bit (derived from the graph at IR→program time) to prebuild it
> conservative instead of failing the plain build. Also fixed: `KOp::StorageLoad` had been MID-INSERTED (shifting every
> later op) → moved to END. **Gate green (real GPUs, MSVC+clang-cl+ASan+shipping): gpu-context-vulkan 201/27 ·
> gpu-context-dx12 151/20 · both backends validation/debug-layer CLEAN · compute byte-exact kir-vulkan 33010 unchanged ·
> kir 390 · shader-helpers 910 · 13 files tidy-clean.**
> **▶ B2 ✅ core (a–d) COMPLETE (2026-07-11) — the TEXTURE & SAMPLER system, both backends.** SEPARABLE texture/sampler IR
> types (`TKind::Texture`/`Sampler`; `TexDim`{1D/2D/3D/Cube} + arrayed/ms/shadow flags + array-count packed in `KType`) — the
> portable model HLSL/WGSL/MSL share; new `KOp` (all appended): `Texture`/`Sampler` leaves · `TexSample`/`SampleLod`/`SampleGrad`/
> `SampleCmp`/`TexelFetch`/`TexGather`/`TexSize`/`SampleIndexed`. **B2-a** 2D sampling foundation — GLSL `texture(sampler2D(t,s),uv)`
> / HLSL `tex.Sample(s,uv)`; NEW device infra (create+upload a 2D RGBA8 image + default sampler; Vulkan set-0 layout gains
> sampled-image(1)+sampler(2), DX12 root sig gains SRV(t1)+sampler(s2) + a sampler heap). **B2-b** the sample-op family
> (explicit LOD/grad · `sampleCmp` shadow via a NEW comparison-sampler + D32/R32F depth-texture path · integer `texelFetch` ·
> 4-texel `gather` w/ literal channel · `textureSize`). **B2-c** dimensions 1D/3D(volume)/Cube(IBL)/2DArray(CSM)/CubeArray
> (Vulkan ONE generalized image creator + layered upload; DX12 per-subresource footprint upload + per-kind SRV ViewDimension);
> **2DMS** emitter wired but observable deferred to a render-to-sampled-MSAA path (MSAA data can't be uploaded). **B2-d**
> BINDLESS — a `texture(...,N)` descriptor array + `tex_sample_at(index)` with a DYNAMIC per-fragment index; GLSL
> `texture2D tex[N]`+`nonuniformEXT` / HLSL `Texture2D tex[N]:register(t3)`+`NonUniformResourceIndex`; Vulkan enables
> descriptor-indexing (set-0 binding 3 = image array[8]), DX12 a t3[8] SRV table (Tier-2). **B2-e** sampler feedback:
> documented as a DX12-only virtual-texturing capability (Vulkan has NO standard equivalent) that also needs mipped textures
> + feedback transcode → rides the texture-streaming system. **EVERY observable is pixel-exact AND IDENTICAL on Vulkan vs
> DX12** (left-red/right-green across all sample ops + dims + bindless; textureSize=16×16; shadow L=white/R=black). ⚠
> **Validation caught 2 REAL VUIDs** (green ≠ clean): a CubeArray view + its `SampledCubeArray` SPIR-V capability need the
> `imageCubeArray` device feature (VUID-viewType-01004 / pCode-08740). **Gate green (real GPUs, MSVC+clang-cl+ASan+shipping):
> gpu-context-vulkan 309/32 · gpu-context-dx12 246/25 · both backends validation/debug-layer CLEAN · compute byte-exact
> kir-vulkan 33010 unchanged · kir 390 · 11 files tidy-clean.**
> **▶ B5 ✅ (a–c) COMPLETE (2026-07-12) — OpenPBR SURFACE MATERIALS, both backends.** New `engine/kir/include/crd/kir/
> ckir_material.hpp` — the MATERIAL-PROFILE layer on core CKIR (ADR-0102 D3: material = surface response, lighting-agnostic;
> the render path lights it at B8). **B5-a** the surface contract + DEFERRED G-BUFFER: the canonical OpenPBR surface struct
> (SROA aggregate) + `pack_gbuffer` (surface → 4 RGBA8 MRT: base_color+metallic · encoded_normal+roughness · emissive+
> occlusion · opacity), and the **NEW MRT device capability** (the resource type ADR-0102 D7 names) — Vulkan dynamic-rendering
> with N colour attachments + N-attachment blend/write dynamic state (`set_draw_state(...,color_attachments)`); DX12 N RTVs in
> one heap + PSO `NumRenderTargets=N` (RT-count in the pso_for cache key). New `IGBufferTarget` + `create_gbuffer_target` +
> `draw_gbuffer` with per-attachment readback. **B5-b** the FULL **OpenPBR 1.1 slab** — a ~37-field append-only struct (base
> extras · specular weight/color/ior/aniso/rotation · transmission weight/color/depth · subsurface weight/color/radius/aniso ·
> coat weight/color/roughness/ior/aniso/darkening · fuzz weight/color/roughness · thin-film weight/thickness/ior · geometry
> thin_walled/tangent/coat_normal · emission_luminance) with spec-correct defaults (`surface_defaults`); an EXTENDED
> 8-attachment G-buffer (`pack_gbuffer_ext`) carries every layer for the observable (the IORs/anisotropy stay in the struct
> for B8's BRDF). **B5-c** a fresh `ShadingModel`{Standard·Unlit·Toon·Cel·Gooch·Outline·Hatching} + `AlphaMode`{Opaque·Masked·
> Translucent·Additive} (none existed) as float-encoded surface tags (packed into gbuf3), + `set_masked` alpha-test discard
> (reuses the B1-b mechanism). **Observables pixel-IDENTICAL Vulkan vs DX12**: a lighting-agnostic material writes its surface
> → G-buffer, every core+slab channel reads back exact (base 204/51/25, metallic 127, normal-z 255, spec_w 153, coat_w 102,
> fuzz_w 204, transmission 64, thin-film 229, subsurface 89, thin_walled 255); the shading-model tag (Gooch=4) reads back
> exact; a masked opacity ramp (cutoff 0.5) discards the sub-cutoff half. ⚠ reconciled with the shipped renderer `SurfaceData`
> APPEND-ONLY stability rule (re-added `opacity`, appended the slab layers — never reorder). **Gate green (real GPUs, MSVC+
> clang-cl+ASan+shipping): gpu-context-vulkan 356/35 · gpu-context-dx12 290/28 · both backends validation/debug-layer CLEAN ·
> compute byte-exact kir-vulkan 33010 unchanged · tidy-clean (SurfaceField enum CamelCase + u8 base per tidy).** **NEXT = B6**
> (MaterialX-parity node library — noise/patterns/operators/UV/NPR, bit-exact vs the MaterialX reference; then B7 lowering →
> B8 renderer integration w/ the GGX/clustered/shadow lighting library, per ADR-0102).

> **═══ RIGHT NOW (2026-07-09): hesap v14–v16 CLOSED (numerical stack + forward/reverse autodiff — detail preserved below).
> Phase 3.1.6 **v17 GPU compute (CKIR)** is the active phase (the kernel/shader COMPILER — ADR-0098/0099/0100). Currently
> on **DETOUR D-007 — CKIR becomes the universal shader IR** (ADR-0101: the IR is the single source of truth for EVERY
> shader; GLSL/HLSL/… are outputs only). **Phase A of D-007 SHIPPED:** the entire GLM-equivalent math/value corpus —
> scalars + comparisons + bit ops + vec2/3/4 + swizzle + geometric + relational + mat3/4 (+det/inv/outer) + interp +
> quaternions — in the IR + CPU oracle (bit-exact) AND running on **Vulkan + DX12** (comps-aware `emit_vec_glsl`/
> `emit_vec_hlsl`). Also shipped this arc: the multi-kernel scheduler (`run_graph`) + full radix sort as one GPU pipeline.
> **A4 CONTROL FLOW COMPLETE** — fixed-count (`unroll_for`) + DYNAMIC (`for_loop` native per-thread GPU loop with divergent
> per-element count + body-scoped LoopIndex/LoopAcc; bounded `while_loop`; `switch`/if via Select), bit-exact on CPU oracle
> + Vulkan + DX12. **PHASE A of D-007 DONE.** Suites: kir 129 · kir-vulkan 32983 · kir-dx12 30804, zero regression. D-007
> now has a SOLID hesap-style subslice plan (A✅ · fan-out · B material B0–B8 · C node editor C1–C5 · D cook D1–D5), refined
> to frontier grade (Slang/MaterialX/OpenPBR-1.1/mesh-shaders). **RENDER-DATA/LIGHTING/PASS ARCHITECTURE DECIDED — ADR-0102**:
> engine ALREADY has a mature renderer (frame graph · Forward/Forward+/Deferred/VisBuffer planned · PerFrameUbo · Material
> Template+variants · cooking) → CKIR replaces the hand-written GLSL SOURCE only; globals live in renderer set-0 NOT the GPU
> context (ADR-0099); frequency sets 0/1/2/3; **material=surface-response lighting-agnostic, render-path=lighting → one
> material Forward+ OR Deferred (hybrid)**; multi-pass=frame graph. **D-007 RE-SCOPED (session close 2026-07-09): D-007 =
> the IR SUBSTRATE = Phase A(✅) + Phase B (material/shading capability incl B9 ray-tracing + NPR/toon) + Phase D (cook).
> FRONT-ENDS (node editor UI + text DSL = Phase C) DEFERRED to the editor phase; authored via C++ builders.** On exit the IR
> can EXPRESS everything (ML/AI/FFT/sim/skinning/particles/lighting/PBR+stylized/RT/effects). **ROADMAP: D-007 → hesap →
> eylem/physics (GPU cloth/deformation/crowds/ragdolls) → rendering → UI → first editor → node editor.**
> **▶ PHASE B STARTED — slice B0 (type system) COMPLETE, B0-0…B0-4 (2026-07-10):** **B0-0** fixed a REAL bug — `KGraph::optimize()`
> never remapped the 4th operand `d`, so any mat4 graph through `optimize()` kept a stale/OOB column (latent; Phase-D cook
> would have hit it); + `operands_valid()` invariant asserted in the pass. **B0-1 `KType`** — one composed type
> `{scalar,kind,rows,cols}` (SPIR-V/Slang model) replaces the `(dtype,comps)` pair; a bare comps count can't tell `vec4`
> from `mat2` and carries no scalar type; full type in the CSE key; ZERO-regression. **B0-2** mat2 + non-square R×C,
> emitters keyed on the TYPE not comps (+ `input_mat`, HLSL `crd_inv2`, generic R×C outer). **B0-3** bool-typed
> comparisons + `bvec`/`ivec`/`uvec` + `DType::U32` (appended, never reordered — the cook serializes it). **B0-4**
> structs + fixed-size arrays: struct registry on KGraph, **VARIADIC operands** (4 slots can't hold N fields; the ext
> pool is walked by DCE/CSE/renumber/emitters — the B0-0 bug class one field out, so `operands_valid()` guards it and
> `clone()` hard-asserts), **SROA lowering** (no GPU struct decl, no std430 — buffer-backed structs are B3). Suites:
> kir **200** · vulkan **33010** · dx12 **30821** · webgpu **30791** · cuda **79944**, every prior assertion intact.
> **⛔⛔ The tidy gate was reporting `clean` for files it NEVER PARSED** (missing `-I` ⇒ 0 diagnostics; `"file not found"`
> filtered out) — `backend_vulkan.cpp` passed the DoD gate un-analysed and **178 violations across crd-kir were
> invisible**. Gate repaired (UNGATED/MISSING = hard fail, globbed includes, PCH-stripped compile DB, prints the file
> count) and all 178 cleared; all 27 crd-kir files clean. **⭐ GLSL is the TYPE-STRICT backend** — `float + bool` passes
> DX12, fails Vulkan; fix the IR, never the emitter. **▶ BACKEND FAN-OUT STARTED (user: all backends, mission overrides
> D-007's "optional")**: **WGSL ✅** type layer on real WebGPU hw (webgpu 30791→**30808**) — `select()` not `?:`, emitted
> `crd_inv2/3` (no `inverse()`), column-built outer (no `outerProduct()`); `For` refused loudly. **CUDA ✅ by
> SCALARIZATION** (no native vec arithmetic; `comps` scalar temps; aggregates FREE via emit-time component resolution —
> even `Select`-of-struct, which GLSL/HLSL SROA must refuse); componentwise ops **bit-exact** (`--fmad=false`);
> cuda 79944→**80599**. Two more real bugs found: **`KirBackendCpu` (the CPU ORACLE) ignored `comps()`** ⇒ vec graphs
> heap-overflowed (the oracle was itself un-oracled — GPU vec tests all used analytic refs); **`graph_uses_vec` was an
> ODR violation** (external linkage in BOTH vulkan+dx12 .cpp) ⇒ hoisted inline.
> **⛔→✅ THE CPU ORACLE WAS NOT f32-FAITHFUL for the A3 vec/mat corpus — FIXED (user-approved).** Componentwise ops
> rounded per step; but Dot/VecLen/Normalize/Cross/MatVecMul/MatMatMul/Determinant/MatInverse/OuterProduct/geometric/
> quats/slerp accumulated in **f64 and rounded only on store**, unlike `Contract`. The oracle was ~1 ULP *more accurate*
> than any f32 kernel ⇒ **ADR-0098's T1 certified-bit-exact core was unreachable for vec/mat** (which is most of a
> shader), and it is why every Vulkan/DX12 vec test compares vs ANALYTIC refs, never vs the oracle. Fixed via
> `eval_detail::rnd` + dtype-threaded `mat_det`/`mat_minor_det`; F64 graphs untouched (round_dtype = identity).
> **PROOF IT BITES: the CUDA `==` asserts failed with 320 mismatches before, pass after.** ★ **PAYOFF: CUDA vec3/mat3/
> bvec/struct now gate BIT-EXACT (`==`) vs the oracle** — the strongest gate in the project (scalarized emitter writes
> the same elementary ops, same order, `--fmad=false`). GLSL/HLSL/WGSL stay ULP-tolerant: their `dot`/`normalize`/
> `inverse` BUILTINS have implementation-defined order — a named, bounded gap for ADR-0098 §5's float_controls audit. **⚠ B1/B2 are UNBUILDABLE before B3** — CKIR is compute-only (no stage concept;
> every emitter hardcodes `local_size_x=256`/`numthreads`), and `dFdx`/`discard` are fragment-stage. **B1↔B3 REORDERED
> (user).** ⚠ **No DX12 raster path exists** (`rhi-vulkan` yes, no `rhi-dx12`) ⇒ B3's DX12 gate = HLSL→DXIL compile; the
> real draw is Vulkan. **MSL ✅** (native float3/float3x3/bool3; emitted crd_inv2/3 + column-built outer — MSL has
> neither `inverse()` nor `outerProduct()`; no Metal compiler off macOS ⇒ STRUCTURAL gate incl. `temps_well_formed`
> (every referenced `tN` declared, no `t-1` — the exact bug GLSL shipped) **and the checker is itself proven to bite**;
> real run = v17-n GH-Actions Apple silicon). **▶ FAN-OUT COMPLETE on every backend reachable from this host** (kir 234).
> **▶ B3-CORE STARTED. B3-a ✅ (IR stage model, kir 254):** `KStage{Compute,Vertex,Fragment}` · `KBuiltin` · leaves
> `StageIn` (location-indexed; ONE op for a vertex attribute AND a fragment interpolant, disambiguated by the entry's
> stage — as SPIR-V models it) · `Builtin` (type fixed by the builtin) · **`UniformBlock` = STRUCT-typed leaf at
> (set,binding)**, members via the existing `field_get` (reuses the B0-4 registry; **`set` IS ADR-0102's frequency slot**);
> `dset` in the CSE key. ⚠ **SCAR:** a stage leaf has NO operands ⇒ const-fold folds it to a constant (a folded
> `gl_VertexIndex` = every vertex is vertex 0). Guarded; **guard proven to bite** — my first test used a vec4 UBO field
> and passed with the guard removed, i.e. tested nothing.
> **▶ B3-a′ ✅ — the stage model is now COMPLETE (kir 400 asserts / 43 cases; `v17` ctest 122/122).** `KStage` = the **14
> SPIR-V execution models** (compute · vertex · tess-control · tess-eval · geometry(legacy) · fragment · task · mesh ·
> raygen · intersection · any-hit · closest-hit · miss · callable) + `stage_mask::*` sets + **32 builtins in ONE
> `builtin_info` table** carrying type AND legal-stage mask together (the old `builtin()` switch had **no `default:` and
> only 4 cases** — any new builtin silently became an f32 scalar) + **`entry_valid(g, e, &why)`**: a graph carries no
> stage, so only an ENTRY can reject `gl_FragCoord` in a vertex shader. Without it the table is a comment. *Proven to
> bite; the same graph is valid as a fragment entry.*
>
> **⛔⛔ THE B3 PLAN WAS WRONG — user direction 2026-07-10 → [ADR-0103] + new detour [D-008].** Gating the raster emitters
> on `crd::shader::compile_glsl` makes **CKIR depend on the module that owns GLSL** — the exact inversion ADR-0101 exists
> to delete. The plan was faithfully following **`ADR-0099 §6`** ("crd-shader stays the single shared GLSL/HLSL→SPIR-V/
> DXIL compiler") — **two ACCEPTED ADRs in direct contradiction.** §6 is now **struck through in place** and superseded by
> **ADR-0103: `crd-gpu-context` owns every GPU program; no module outside a backend names a shading language or a
> bytecode.** Currency IN = the IR (`KGraph`+`KEntry`), OUT = an opaque `IGpuProgram`; `compute()`/`raster()`/
> `raytracing()`; each backend owns its language + compiler privately. Three leaks MEASURED: `compile_glsl`/`compile_hlsl`
> (9 call sites) · `rhi::ShaderModuleDesc::code` = raw SPIR-V in a public header (38 sites/9 files) · two device layers
> over two `VkDevice`s. **NEXT: D-008 C0** (program seam; delete `crd/shader/compile.hpp`; I1/I2 grep-gate) → C1
> `IRasterContext` → C2 absorb `rhi-vulkan`'s device → C3 RT → C4 DX12 raster; **then** D-007 B3-c (GLSL VS+FS emitters,
> first hoisting the ~60-case value switch into a shared statement emitter), B3-d (HLSL), B3-e (the draw), then B1.
> Close B0 with the multi-config DoD.
> ⚠ **A green test BINARY is not a green ctest:** `crd-kir-tests.exe` said "All tests passed" while 6 cases could not be
> *selected* by ctest (em-dash names + Windows Active-Code-Page argv) and the registered guard `crd-no-non-ascii-test-names`
> was RED. 9 names fixed; guard green. Guards are ctest-registered — run `scripts/run-ctest.bat`, never the binary.
> **▶ D-008 C0 ✅ — the program seam landed (2026-07-10).** The two SPIR-V compilers moved `crd-shader` →
> `gpu-context-vulkan` (`crd::gpu::compile_glsl_to_spirv`/`compile_hlsl_to_spirv`); **`crd/shader/compile.hpp` DELETED**;
> `crd-kir-vulkan` no longer links `crd-shader`; 7 call sites + 4 CMakes migrated. `crd/gpu/program.hpp` = `ShaderStage`
> (14) + opaque `IGpuProgram`; **`IGpuContext::create_program(cooked SPIR-V) → IGpuProgram`** implemented in Vulkan +
> **tested end-to-end** (compile → program → valid; malformed rejected). **I1 grep-gate** `crd-no-shader-language-leak`
> (ctest-registered) green. `test_ckir_glsl` relocated to `tests/gpu-context-vulkan` (keeps crd-kir's link-isolation
> smoke). Counts EXACT: kir-vulkan **33010** · kir-dx12 **30821** · geometry-bvh-gpu **851016** · rhi_vulkan **4819**;
> conserved relocation kir 400/43→**390/40**, gpu-context-vulkan 8/1→**24/5**. All tidy-clean. ⚠ **Deeper leak surfaced +
> tracked:** `crd-shader/src/runtime.cpp` (the Effect/Module RENDERING frontend) STILL compiles GLSL via shaderc —
> allowlisted in the gate, migrates at **C2** (the rendering convergence). **NEXT: D-008 C1** (`IRasterContext` +
> `create_program(KGraph,KEntry)`; raster path needs D-007 B3-c) → C2 (absorb rhi-vulkan's device + empty the allowlist).
> **▶ FRONTIER AUDIT (2026-07-10, web-grounded):** the C-slices + D-007 B-slices were extended to cover the 2024–26 state
> of the art — **shader objects** (`VK_EXT_shader_object`, kills PSO permutation explosion — a C1 *design* decision) ·
> **bindless / descriptor buffers** · **dynamic rendering** · VRS/ROV/conservative-raster · **device-generated commands**
> (`VK_EXT_device_generated_commands`) → **work graphs** (`VK_AMDX_shader_enqueue`, D-008 **C5**) · **SER**
> (`VK_EXT_ray_tracing_invocation_reorder`) + **opacity micromaps** + **cluster AS / RTX Mega Geometry**
> (`VK_NV_cluster_acceleration_structure`, D-008 **C3**) · **cooperative vectors / NEURAL shading** (`VK_NV_cooperative_vector`,
> D-008 **C6** + D-007 **B10** — differentiable-by-construction via v15/v16 autodiff, a moat nobody ships) · subgroup/wave +
> **work-graph node shaders** (D-007 **B11**). Full extension list + citations + slice mapping in the D-007/D-008 frontier
> tables. D-008 is now **C0✅ → C6**.
> **▶ D-008 C1-a ✅ (2026-07-10) — the raster foundation.** `VulkanGpuContext` now creates a GRAPHICS queue (distinct
> from the dedicated async-compute queue: graphics_family=0, compute_family=2) + enables dynamic rendering &
> **`VK_EXT_shader_object`** (guarded; shader_object=YES on the 4070 Ti Super) — the ADR-0099 "one device, both concerns"
> made real. Backend-agnostic **`IRasterContext`/`IRasterTarget`** (shader-object-shaped, no Vulkan in the interface) +
> Vulkan impl: offscreen RGBA8 target + **dynamic-rendering CLEAR + pixel readback**, green on real GPU (raw Vulkan, NO
> crd-rhi edge — C2 absorbs rhi, the edge can't point back). kir-vulkan **33010** unchanged; gpu-context-vulkan
> 24/5→**36/6**; tidy + I1 + ASCII gates green.
> **▶ D-008 C1-b ✅ (2026-07-10) — the shader-object DRAW.** `IGpuProgram` retains its cooked SPIR-V
> (`VulkanGpuProgram::vk_spirv()`); `IRasterProgram` = a linked VS+FS as **`VkShaderEXT`** (`vkCreateShadersEXT`,
> `LINK_STAGE`); `IRasterContext::create_raster_program` + `draw` set all ~18 shader-object dynamic-state fields (NO PSO)
> + `vkCmdDraw` on dynamic rendering. An **attributeless red triangle over a blue clear renders green on real GPU** —
> centre pixel RED, corner BLUE — from trivial cooked VS/FS via the relocated compiler (no B3-c). The whole seam proven:
> GLSL→SPIR-V→IGpuProgram→shader objects→draw→readback. gpu-context-vulkan 36/6→**48/7**; kir-vulkan **33010** unchanged.
> **▶ D-008 C1-c ✅ (2026-07-10) — the IR on-ramp.** `IGpuContext::create_program(const KGraph&, const KEntry&)` (kir
> types forward-declared in the base header; only `gpu-context-vulkan` links **crd-kir** — the acyclic ADR-0103 edge).
> Compute: emit GLSL via crd-kir's `emit_(vec|elementwise)_glsl` → SPIR-V via the relocated compiler → `IGpuProgram`;
> raster refused (B3-c). Proven: a compute KGraph `(x+y)*exp(x)` → valid program w/ real SPIR-V; a vertex entry → nullptr.
> gpu-context-vulkan 48/7→**53/8**; kir-vulkan **33010** unchanged; tidy + I1 + ASCII gates green. (Dispatch-through-the-seam
> = the kir-vulkan convergence, later.)
> **⛔→ C1-d RECLASSIFIED into C2 (scouting finding):** renderer/draw/sandbox all run on `crd::rhi::Device` — a SEPARATE
> `VkDevice` from `VulkanGpuContext`'s; handles can't cross devices, so "renderer consumes IRasterContext" is inseparable
> from C2's device unification. **C1's raster surface is COMPLETE at a/b/c.** User chose **C2 (renderer device
> absorption)**; decomposed into C2-a…C2-f (each incremental so the working sandbox never breaks).
> **▶ D-008 C2-a ✅ (2026-07-10):** `VulkanGpuContext` is now RENDER-CAPABLE — `headless=false` enables surface
> (`VK_KHR_surface`+platform) + `VK_KHR_swapchain` when available; `render_capable()` = surface+swapchain+graphics queue.
> Guarded + additive (headless/compute byte-for-byte unchanged). gpu-context-vulkan 53/8→**59/9**; kir-vulkan **33010**
> unchanged.
> **▶ D-008 C2-b ✅ (2026-07-10) — rhi-vulkan ADOPTS the gpu-context device (ONE VkDevice).**
> `crd::rhi::create_vulkan_device_adopting(IGpuContext&)` builds a `VulkanDevice` over the context's
> instance/physical/device/queues (downcast in the .cpp; header stays abstract via forward-decl). `VulkanDevice` gained
> `owns_device` — an adopted device frees ITS pools/allocations but **never destroys the shared VkDevice**. Edge
> `rhi-vulkan → gpu-context-vulkan` (PRIVATE, acyclic — build-verified). Proven: adopt → device stands up on the shared
> VkDevice; destroy it → device still alive (re-adoption succeeds). rhi_vulkan 4819/25→**4823/26**; kir-vulkan **33010**
> unchanged; **sandbox links** (edge propagates).
> **▶ D-008 C2-c1 ✅ (2026-07-10) — the adopted device is FEATURE-MATCHED.** Scouting found rhi's own device enables
> **synchronization2 + fillModeNonSolid** (its render path needs them) that the windowed context lacked. A windowed
> `VulkanGpuContext` now enables both (guarded — headless/compute byte-for-byte unchanged); `create_vulkan_device_adopting`
> passes `sync2 = render_capable()`. So the renderer can run on the adopted device unchanged. Tested: windowed context →
> render_capable → adopt → feature-complete rhi Device. gpu-context-vulkan **59/9**, kir-vulkan **33010**, both unchanged.
> **⚠ C2-c2 blocker found:** `crd::imgui::ImGuiLayer` takes a `crd::rhi::Instance&` + pulls VkInstance via
> `crd::rhi::vulkan_instance()` — the adopted path has NO rhi Instance.
> **▶ D-008 C2-c2 ✅ (2026-07-10) — THE SANDBOX RENDERS ON ONE DEVICE.** (1) Decoupled ImGui: added
> `crd::rhi::vulkan_instance(Device&)` (VkInstance from `VulkanDevice::instance()`); `ImGuiLayer` dropped its
> `crd::rhi::Instance&` param, gets the instance from the Device. (2) Swapped the sandbox bring-up:
> `create_vulkan_instance`+`create_device` → `create_vulkan_gpu_context({headless=false})` + `create_vulkan_device_adopting`
> (`gpu_context` declared first, outlives `device`). Renderer + swapchain + ImGui + frame graph all on the ONE
> VulkanGpuContext device. **Sandbox smoke PASS: 436 frames presented over 3.0s @ 145 fps, validation ON, no VUID.**
> rhi_vulkan **4824/26** · kir-vulkan **33010** unchanged. ⚠ `vulkan_native.hpp` standalone-UNGATED (pre-existing GLFW
> include); content gated via imgui_layer.cpp + vulkan_backend.cpp (clean). **The two VkDevices the ADR-0099 audit found
> are now ONE.** **▶ D-008 C2-d1 ✅ (2026-07-10) — the opaque-program pipeline path (I2).** `rhi::GraphicsPipelineDesc` gains
> `vertex_program`/`fragment_program` (`crd::gpu::IGpuProgram*`, forward-declared — rhi stays abstract), APPENDED at the
> struct end so positional inits are unaffected. `VulkanGpuProgram::vk_module()` exposes the compiled module;
> `create_graphics_pipeline` resolves a stage from the program (wins) or the legacy `ShaderModule` (strangler-fig — existing
> consumers untouched). Proven: a pipeline builds from opaque VS+FS programs; **sandbox still renders** (292 frames);
> rhi_vulkan **4830/27**, kir-vulkan **33010**. (Direct-tidy of types.hpp surfaced 3 PRE-EXISTING enum-size warnings on
> flag/format enums — justified NOLINTs added.) **▶ D-008 C2-d2 ✅ (2026-07-11) — the migration facade + first consumer.** `rhi::Device::create_program(ShaderStage,
> cooked SPIR-V) → IGpuProgram` (NON-pure default nullptr; the ADOPTED `VulkanDevice` delegates to its owning
> `VulkanGpuContext`, mapping the stage). So any consumer holding a `Device` mints opaque programs — no gpu-context
> threading. Edge `crd-rhi → crd-gpu-context` (header-only, acyclic). **First consumer: renderer `ForwardRenderPath`** —
> all 3 `create_shader_module` → `create_program` + `vertex_program`/`fragment_program`. Sandbox renders **529 frames @
> 176 fps, no VUID**; renderer 192/40 · rhi 152/32 · rhi_vulkan 4830/27 · kir-vulkan 33010 green. **▶ D-008 C2-f ✅ (2026-07-11) — DEVICE CONVERGENCE CLOSED; ONE VkDevice everywhere.** Deleted
> `VulkanInstance::create_device` (the ~150-line VkDevice-creation path) and retired the `Instance::create_device`
> interface method. **rhi-vulkan creates NO VkDevice** — every `crd::rhi::Device` is adopted from a `VulkanGpuContext`
> (`create_vulkan_device_adopting`). Migrated all ~26 `test_rhi_vulkan` sites + 2 geometry-shader-helpers to an
> `AdoptedGpu` helper (headless compute AND windowed swapchain/present, both from a gpu-context); added a
> `ValidationCapture(Device&)` overload so the VUID gate attaches to the adopted device's VkInstance. `Instance` keeps only
> `enumerate_adapters`. No test needed `RequireDedicated`, and the gpu-context detects a dedicated compute family exactly
> like rhi did — so async-queue semantics are preserved. rhi_vulkan **4806/27** (compute, async queues, ValidationCapture,
> windowed swapchain/triangle — all adopted) · geometry 910 · rhi-mock 151 · kir-vulkan 33010 · renderer 192 · shader 139 ·
> leak gate green · sandbox smoke PASS; MSVC + clang-cl clean; 8 files tidy-clean. **The full per-slice sweep also surfaced
> 8 stale `runtime/examples/smoke_*` programs** broken by the ACCUMULATED D-008 API changes (deleted `compile.hpp`, retired
> `ShaderModule`, `create_runtime(compiler)`, retired `create_device`) — all migrated (adopt device / create_program / inject
> compiler / compile_glsl_to_spirv) + CMake deps added; full win-debug builds clean, all 8 tidy-clean. Sweep verdict:
> win-debug/asan/shipping **100% ctests (4763/4763/4676)**. The full win-tidy config (first clean run since the clang-tidy
> 20.1.8 + MSVC 14.51 bumps) also peeled a tail of PRE-EXISTING toolchain-surfaced tidy debt — cleared: disabled
> `readability-redundant-casting` (false positive on MSVC's `<xutility>`), fixed dx12 `bugprone-branch-clone` +
> `bugprone-casting-through-void`, and `readability-identifier-naming` in hesap-tensor + jobs tests (see
> [ninja -k 0 onion note]). **D-008 CONVERGENCE COMPLETE**
> (I1+I2 both closed, one device). **▶ SEQUENCING LOCKED (user 2026-07-11): build the FULL visual shader frontier FIRST,
> THEN hesap-GPU.** Order: **B3-c raster → B1/B2 materials+lighting → B4 mesh → B9+C3 ray tracing → B10 NEURAL →
> B11+C5 work-graphs → Phase D cook → hesap-GPU** (the last stop). The compute substrate hesap needs is ALREADY proven
> (GEMM cuBLAS parity, 6 backends), so hesap-GPU is a dependency-light return — but the user chose to complete the whole
> visual frontier first. **NEXT: D-007 B3-c** (raster VS+FS emitters behind the seam — start HERE, not C3; C3 is the RT
> device context that pairs with B9 later). See [[project_full_visual_frontier_before_hesap_gpu]].
>
> **Prior: D-008 C2-e ✅ — I1 FULLY CLOSED; crd-shader owns NO shading language.** The Effect frontend
> (`runtime.cpp`) dropped its shaderc loader entirely and now takes an INJECTED `crd::shader::ISpirvCompiler`. New bridge
> module **`crd-shader-vulkan`** (`create_vulkan_spirv_compiler`) wraps `crd::gpu::compile_glsl_to_spirv` — it links BOTH
> crd-shader + crd-gpu-context-vulkan so neither depends on the other (gpu-context stays rhi-free). Compiler called with a
> new `optimize=false` flag so `OpName`s + dead bindings survive for spirv-reflect. Callers (sandbox, test_renderer, the
> crd-shader test suite) inject the compiler, declared before the runtime they borrow it into. **`crd-no-shader-language-leak`
> gate GREEN with an EMPTY allowlist.** crd-shader-tests 139/21 · renderer 192/40 · resources 12301/78 · kir-vulkan 33010 ·
> geometry 910 · sandbox smoke PASS; 10 files tidy-clean; MSVC + clang-cl clean. **NEXT: C2-f** retire rhi's own device
> creation (the standalone `create_device` path) → then D-007 resumes at B3-c.
>
> **Prior: D-008 C2-d4 ✅ — `ShaderModule` RETIRED, I2 FULLY CLOSED.** Deleted `crd/rhi/shader_module.hpp`
> (`ShaderModule`), `ShaderModuleDesc`, `Device::create_shader_module`, the graphics `*_shader` fields, and
> `VulkanShaderModule`. **Compute migrated too**: `ComputePipelineDesc.compute_shader`→`compute_program` (opaque
> `IGpuProgram`). The standalone rhi device mints programs via a new `crd::gpu::make_vulkan_program(VkDevice,…)` factory —
> the SAME constructor `IGpuContext::create_program` uses (gpu-context-vulkan still OWNS authoring per ADR-0103), so
> `test_rhi_vulkan` kept its device + queue-selection untouched (no perturbation of the async-compute queue tests). The
> Vulkan HLSL compiler now normalizes the SPIR-V entry to `main` (`-fspv-entrypoint-name=main`), so every minted program
> entry-points at `main` and the pipeline needs no source function name (matches the GLSL/IR path). **No raw SPIR-V in any
> public rhi header** — `crd-no-shader-language-leak` gate GREEN. rhi 152/32 · renderer 192/40 · rhi_vulkan **4830/27**
> (compute+graphics on the standalone device) · geometry-shader-helpers **910/21** (21 HLSL manifests dispatch with the
> normalized entry) · kir-vulkan 33010 · sandbox smoke PASS. 14 files tidy-clean; MSVC + clang-cl both build clean (gcc
> leg env-blocked: WSL dir lacks spirv-reflect source — pre-existing, unrelated). **NEXT: C2-e** empty the I1 allowlist
> (migrate crd-shader `Effect` frontend / `runtime.cpp` off shaderc — the last hand-written GLSL) → **C2-f** retire rhi's
> own device creation.
> Docs: **`docs/detours/D-007-gpu-program-system.md`** (the MERGED master — D-007 + D-008 in one ordered subslice table;
> the old `D-007-ckir-*` / `D-008-gpu-context-*` are redirect stubs), `docs/decisions/0101-*.md` + `0102-*.md` +
> **`0103-gpu-context-owns-every-gpu-program.md`**, `docs/sessions/2026-07-09-d007-*.md` + **`2026-07-10-d007-b0-type-system.md`**. ═══**
>
> **v14 IS DONE.** Same-evening closes on top of the wave below: **v14-m** (NN inference —
> f32 8/8, q8 vs torch-int8/ort-int8/ggml-from-source ALL WON incl. the last cell via the
> per-tensor i8 tier 1.23× at better accuracy; 18 cases/5,194 asserts) · **v14-z** (12-command
> `hesap.tensor.*` CLI + test_cli 5/5 · `docs/systems/hesap-tensor.md` · ADR-0096 amendments ·
> the ALL-PEERS SCOREBOARD `docs/bench/2026-07-05-v14z-scoreboard.md` + conformance audit) ·
> **TBLIS + xtensor measured at last** (TTGT case 1.14× WIN; 3 pure-GEMM rows = the named v0d
> gap; xtensor 3/3) — **FINAL: 210 rows, 199 won, 7 tie/parity, 4 open = ONE named bug (v0d
> raw f64 GEMM kernel, ADR-0100 proposal on file).** Module ctest 133/133 env-free.
> **PENDING (deferred, not blocking): the whole-engine per-slice-check sweep (→ CI, per user) +
> the combined commit (v10 FFT close + v14 g–z; user commits).**
>
> **v15 (AUTODIFF I — forward mode) KICKED OFF 2026-07-06.** New module `crd-hesap-autodiff` (shared with v16).
> **ADR-0097 Accepted** (the v15+v16 pair — one module fwd+rev, the deterministic no-atomics tape = v16's crown,
> `Dual<T>` migrates out of hesap-opt with a zero-regression gate, autodiff-lower-than-solvers edges, Enzyme OUT,
> tape→C++ codegen graceful-gated, the 3-oracle gate). Detail doc `docs/phases/phase-3.1.6-v15.md`. Forward-AD
> **peer environment stood up** under `external/` (Ceres Jets / autodiff.hpp / CoDiPack / Sacado / Adept / JAX-CPU
> / ColPack; the HPTT/ReproBLAS WSL convention; `external/PEER_ORACLES.md`). Frontier crush levers + reconstruct-verify
> formula tables captured in `docs/research/2026-07-06-v15-forward-ad-crush.md` (the a–z impl reference).
>
> **v15-a substrate + Dual migration LANDED (2026-07-06, win-debug green).** Module `crd-hesap-autodiff` created
> (`autodiff/{dual,jet,forward}.hpp`); `Dual<T>` migrated VERBATIM (ns `autodiff::forward`), opt re-exports via shims;
> `Jet<T,N>` (no-Eigen, one-pass N partials) + `select`/`min`/`max` added; edge `hesap-opt → hesap-autodiff` wired.
> **ZERO-REGRESSION GATE GREEN**: opt suite 3782 assertions / 152 cases UNCHANGED. **▶ SIMD CARRIER `jet_simd.hpp`
> (`JetPackD<N>`, Vec4d recursive named-register pack + FMA-order, sincos-fused) + FULL 7-PEER BOARD** (+ Sacado[apt]
> + Adept[from-source]): **N=4 beats ALL 7 peers (Sacado 10.8×/Adept 24×); N=16 beats Ceres 1.24×; n-pass/Trilinos/
> Adept field crushed 2–24× everywhere.** Array-LOSS→WIN@N=16 via named-register(SROA)+FMA-order (Fable-consulted).
> **N=8 vs Ceres/CoDiPack-Vec = OPEN crush target (never traded for determinism).** Correct ≤1ulp + deterministic +
> MSVC/clang-cl/gcc/asan green (autodiff 195/25). Board `docs/bench/2026-07-06-v15a-forward-carrier.md`.
> **v15-a substrate + carrier DONE — 6-config DoD GREEN** (win-debug/asan/shipping/tidy + clang-cl + gcc; opt
> zero-regression; math-mandate + name-check PASS).
> **v15-b DONE — cmath JVP rule library + 3-oracle gate + CRUSH.** `detail/jvp_rules.hpp` (each slope once) + full
> surface on Dual+Jet + hardened `pow` (branchless Ceres slope; NaN only at the (0,0) singularity — ⛔ a branched
> version tripped an MSVC /O2 conditional miscompile) + `gradient_check.hpp` (analytic≡cstep≡FD). **CRUSH: tanh-MLP
> 5.15×/4.39×/3.73× vs Ceres @ N=4/8/16, 5.17×/4.97×/3.48× vs CoDiPack** (`crd_v15a_batched_bench`). 238 asserts/32;
> 6-config DoD GREEN; opt zero-regression re-held.
> **v15-c DONE — exact 2nd-order (hyper-dual) + CRUSH.** `hyperdual.hpp`: flat `HyperDual<T>={f0,f1,f2,f12}`
> (Fike-Alonso) + drivers `hessian`/`curvature` (vᵀHv one pass = TR/Newton lever) + nested `Dual<Dual>` cross-check +
> exact-Hessian Newton gate. **CRUSH (batched curvature): 13.1×/13.3×/12.9× vs autodiff `dual2nd`, 17.2×/16.7×/16.2×
> vs FD-of-FD @ N=4/8/16** (`crd_v15c_hyperdual_bench`), matched accuracy. 266 asserts/39; 6-config DoD GREEN; opt
> zero-regression.
> **v15-d DONE — forward drivers + runtime-n tiling + CRUSH.** `drivers.hpp`: `gradient`/`jacobian`/`jvp`/
> `directional` over `JetPackD<W>`, runtime-n tiling (ragged tail), allocation-free, no Eigen. Determinism moat:
> gradient BIT-IDENTICAL across tile widths W=4/8/16 (= {1..16}-worker moat by construction; explicit sweep → v15-z).
> **CRUSH (batched dense Jacobian, fairness-gated bit-exact): 4.06×/2.90×/2.57× vs Ceres, 4.64×/3.50×/3.46× vs
> CoDiPack @ N=4/8/16** (`crd_v15d_drivers_bench`). 315 asserts/44; 6-config DoD GREEN; opt zero-regression.
> **v15-e DONE — automatic sparsity detection + coloring + recovery, BOTH crushes.** `sparsity.hpp` (`JacPattern<W>`
> global tracer + `JacLocal<W>` local value+set tracer — deterministic/alloc-free) + `sparse_jacobian.hpp`
> (trace→distance-2 color [= valid star coloring]→CSR O(nnz) recovery) + `sparsity_hessian.hpp` (5-flag, verified) +
> `sparse_hessian.hpp` (`HessRow<W>` ε2-tiled recovery, W entries/pass, ≡ dense hyper-dual bit-exact). **CRUSH
> (fairness-gated bit-exact): sparse Jacobian 2.5×/6.0×/11.7× vs Ceres-dense; sparse HESSIAN 13.3×/26.9×/40.7× vs
> dense hyper-dual — BOTH GROWING.** Minimal star coloring + B=H·S HVP recovery = v16-e (forward-over-reverse; ships
> with its consumer). 6-config DoD GREEN (1070 asserts/51); opt zero-regression. `docs/bench/2026-07-06-v15e-sparsity.md`.
> **v15-f DONE — matrix-calculus + suite JVPs.** `matrix_jvp.hpp` (self-contained factor-reuse rules: gemm/solve/
> cholesky/logdet/eigvals/svdvals) + `suite_jvp.hpp` (FFT linear / conv filter / Thomas spline). **HONESTY:**
> value-only logdet/eigvals/svdvals finite at repeated λ where JAX/PyTorch NaN. **CRUSH (factor-reuse, fairness-gated
> bit-exact): ∂x/∂b=A⁻¹ — 3.4×/8.3×/19.3× vs AD-through-Cholesky @ n=16/32/64, GROWING** (O(n³) vs O(n⁴)). 6-config
> GREEN (1128 asserts/59); opt zero-regression. `docs/bench/2026-07-06-v15f-matrix-jvp.md`. **NEXT: v15-g** (Taylor-mode
> jets + Taylor ODE integrator).
> **v15-g DONE — Taylor-mode jets + Taylor ODE integrator (regime-honest crush).** `taylor.hpp` (`TaylorJet<T,K>`
> normalized coeffs + master recurrence, O(K²), coeffs≡analytic) + `taylor_ode.hpp` (order-by-order + Jorba-Zou
> adaptive; ≡ closed-form ODEs) + `taylor_tape.hpp` (**O(K²)/step TIDES-class taped integrator**: record RHS op-graph
> once, propagate coeffs order-by-order; ~K× faster than the generic O(K³) build). **CRUSH:** (1) JETS — high-order
> derivs Taylor-EXACT vs FD-garbage (3.7e3 err) + O(K²) vs nested-AD O(2^K) — total, no caveat; (2) ODE work-precision
> (taped O(K²) vs adaptive DP45) — **Taylor CRUSHES 2.6×@1e-9, 10×@1e-12** + MORE ACCURATE at EVERY tol (36× @1e-3);
> crossover ~1e-7, loose-tol = simple-stepper regime. ⚠ fixed a real oscillatory-divergence step-control bug. 6-config
> GREEN (1170 asserts/63); opt zero-regression.
> **v15-h DONE — complex/Wirtinger forward (capability crush).** `complex_dual.hpp`: holomorphic dual =
> `Dual<std::complex<T>>` (holomorphic ops free via complex multiply) + non-holomorphic conj/Re/Im/abs/norm + Wirtinger
> `(∂/∂z,∂/∂z̄)` via seeds ż=1,ż=i + CR gate; deterministic complex `sincos` added to crd/math. **CRUSH:** exact complex
> sensitivities — holomorphic `∂sin(H)/∂h_m` EXACT 5.6e-17 (1 pass) vs FD ~1e-10; Wirtinger exact (2 passes); real-only
> AD can't, JAX non-deterministic. 6-config GREEN (1204 asserts/67); opt zero-regression.
> `docs/bench/2026-07-06-v15h-complex-wirtinger.md`.
> **v15-z DONE — v15 FORWARD-MODE CLUSTER COMPLETE.** Shipped `hesap.ad.*` CLI (gradient/hessian/taylor via canned
> functions; `cli_register_autodiff.cpp` + `cli_anchor.hpp`; acyclic edge hesap-autodiff→crd-hesap) + conformance test
> + system doc `docs/systems/hesap-autodiff.md` + ADR-0097 finalized + **FULL CRUSH SCOREBOARD**
> `docs/bench/2026-07-06-v15z-scoreboard.md`. 1249 asserts/72 GREEN (win-debug). ⚠ Per user plan the **6-config DoD +
> {1..16} moat sweep is BATCHED WITH v16** (2-config after v16), NOT run now.
> **v16 KICKED OFF (2026-07-06)** — deep research (`docs/research/2026-07-06-v16-reverse-ad-crush.md`) validated the
> a–z plan vs 2025–26 SOTA + folded in NEW parts: **batch-invariant reductions** (Sep-2025: non-determinism =
> batch-size-dependent reductions) · **local-adjoint preaccumulation** · **bicoloring** · **sparse reverse LA** ·
> **discretize-then-optimize vs continuous-adjoint** honesty split · **Alt-Diff** · **Efficient-KAN**. Detail doc
> `docs/phases/phase-3.1.6-v16.md`; master rows updated; 12 tasks (#10–21). MOAT: bit-identical {1..16}+batch-invariant
> gradients = deterministic training (torch/JAX can't — atomic scatter-add).
> **v16-a DONE — deterministic reverse-mode tape.** `tape.hpp` (SoA arena Wengert `Tape`+`Var`; VJP = transpose of the
> v15 JVP slopes; fixed-order no-atomics `backward()`) + `reverse.hpp` (`gradient`/`jacobian`/`batch_gradient`). **MOAT:
> batched ∇ BIT-IDENTICAL across {1,2,4} workers** (real crd-jobs parallelism, fixed-order fold). **CRUSH:** full ∇f in
> one O(n) pass — 5.2× vs forward-SIMD, 22.2× vs FD @ n=1024, growing (forward wins small-n = its regime, honest). New
> edge autodiff→crd-jobs. 1276 asserts/76 GREEN (win-debug). `docs/bench/2026-07-06-v16a-reverse-tape.md`.
> **v16-b DONE — scalar VJP rule library.** `rules_reverse.hpp`: full crd::math surface reversed + binary
> (atan2/hypot/pow) + control flow (abs/min/max/select). **Each partial REUSES the v15 forward slope — VJP=transpose
> of JVP.** 3-oracle gate (reverse≡forward-JVP≡FD, 66 asserts). **CRUSH:** transcendental gradient MACHINE-EXACT
> 2.2e-16 one pass vs FD ~3e-9. 1342 asserts/79 GREEN. `docs/bench/2026-07-06-v16b-reverse-rules.md`.
> **v16-c DONE (2026-07-06).** Full v14-m NN VJP set trainable in-engine: `nn_reverse.hpp` (matmul/bias/ReLU/softmax-CE
> + conv2d/max+avg-pool/LayerNorm/GELU/tanh/sigmoid/softmax) — full CNN one-pass gradcheck ≡ FD; `einsum_reverse.hpp`
> (einsum VJP on the real v14 `EinsumPlan`, header-only bridge, `==nn::matmul_vjp`); `bicoloring.hpp` (bidirectional
> coloring — **arrowhead 17→3 sweeps**, ≡ analytic); `sparse_reverse.hpp` (CSR spmv/spmm/solve VJPs wrt dense+sparse
> entries — **capability torch/TF lack**). **Full autodiff suite 2534 asserts/91 GREEN** (win-debug). **▶▶ CLOSE CRUSH —
> torch+JAX value+grad PARITY & FASTER** (WSL 1T f64, matched): loss+grads bit-match Cerid/torch-2.12/JAX-0.10; **MLP
> 55µs vs torch 88µs (1.59×)/JAX 70µs (1.27×); CNN 458µs vs torch 1230µs (2.69×)/JAX 764µs (1.67×)** — rides the v14
> hesap-dense GEMM (matmul VJP = GEMM); + `{1..16}` deterministic-gradient moat torch/JAX lack. Boards
> `docs/bench/2026-07-06-v16c-{nn-vjp,bicolor-sparse}.md`. ColPack (built) TIES bicoloring at 3 seeds (honest; edge =
> integrated deterministic pipeline).
> **v16-d DONE (2026-07-06).** Matrix-calculus + suite VJPs (exact transpose of v15-f): `matrix_reverse.hpp`
> (gemm/solve[LU+SPD]/chol/logdet/eigvals/svdvals, factor-reuse, value-only) + `suite_reverse.hpp` (FFT-VJP=adjoint
> DFT=IFFT/DSP-filter/spline-Thomas). Gate = adjoint identity `⟨ȳ,JVP(v)⟩==⟨VJP(ȳ),v⟩` vs FD-gated v15-f JVPs +
> value-only degeneracy + Jacobi eig/SVD (recon≡A). **Suite 2729 asserts/101 GREEN.** **▶▶ CRUSH: JAX value+grad PARITY**
> (solve/logdet/svdvals/eigvals/fft, matched f64 10–12 digits) **+ SOLVE 3.77× FASTER than JAX** (factor-reuse+native) +
> FFT-VJP=IFFT bit-parity + determinism moat. HONEST: value-only svdvals grad = parity with JAX/torch value-only (both
> finite); only full-SVD U/V path NaNs (torch). Board `docs/bench/2026-07-06-v16d-matrix-suite.md`.
> **v16-e DONE (2026-07-06).** Forward-over-reverse HVP: `hvp.hpp` — generic tape `RTape<T>` with `T=Dual<f64>` (seed
> leaf `Dual{x_i,v_i}` → grad in `.v`, exact `∇²f·v` in `.d`, one fwd build + one backward) + Hessian-free
> `newton_cg_step` (CG matvec=HVP). Production f64 tape untouched. Gate ≡ v15-c hyper-dual exact `H·v`+`vᵀHv`+FD+
> determinism; Newton-CG exact-in-1-step on a quadratic. **Suite 2767 asserts/104 GREEN.** **▶▶ CRUSH: JAX `hvp`/torch
> functorch PARITY** (10-digit) **+ crushes torch functorch 32–640× + beats JAX 2.6–4.1× in the opt regime (n≤~700)**
> (scalar tape `hvp.hpp` = arbitrary functors). **★★ n=1024 GAP CLOSED — `vhvp.hpp` VECTORIZED forward-over-reverse HVP**
> (tape of VECTOR ops, n-wide SoA DualVec auto-vectorized to SIMD, reductions=vectorizable sum+broadcast; gated ≡ scalar
> HVP ≡ hyper-dual `H·v` ≡ FD) **BEATS JAX at EVERY n: 6.8×/4.5×/1.22× (n=64/256/1024), reversing the 0.49× loss** at
> exact parity — down-payment on v16-h. + determinism moat. 3rd-order consumer-gated per plan. Suite **2803/105**.
> Board `docs/bench/2026-07-06-v16e-hvp.md`.
> **v16-f DONE (2026-07-07).** Revolve checkpointing + ODE-adjoint. `revolve.hpp` — GW-optimal treeverse via a memoized
> DP (O(log T) memory, static/WCET schedule). `ode_adjoint.hpp` over a self-contained RK4: **DTO** (AD-through the
> integrator, per-step so revolve checkpoints forward states — EXACT, the default) + **CTO** (continuous adjoint, the
> caveated path). Gate: revolve VALID+GW-OPTIMAL; DTO ≡ FD (exact); revolve-DTO == store-all BIT-IDENTICAL; CTO
> approximate. **Suite 2850 asserts/108 GREEN.** **▶▶ CRUSH (vs torchdiffeq 0.2.5 rk4):** DTO grad PARITY (10-digit,
> both ≡FD) **+ 607–777× FASTER** (Cerid 48µs/311µs vs torchdiffeq 37.5ms/189ms) **+ EXACT AND O(log T) memory in one
> path** (torchdiffeq forces exact-`odeint`-O(T) XOR inexact-`odeint_adjoint`-O(1)) + determinism moat. CTO honesty
> caveat (|CTO−FD|=1.87e-5). Board `docs/bench/2026-07-07-v16f-ode-adjoint.md`.
> **v16-g DONE (2026-07-07).** The implicit-diff suite — differentiate the SOLUTION via the IFT, never unroll.
> `implicit_diff.hpp`: **root_vjp** (F(x*,θ)=0), **fixed_point_vjp** (x*=g(x*,θ)), **qp_eq_vjp** (equality QP via KKT,
> OptNet). Backward=O(1) solves, factor-reuse, deterministic. Gate: all ≡ FD of the re-solved problem. **Suite 2869
> asserts/111 GREEN.** **▶▶ CRUSH:** jaxopt PARITY (10-digit) +214× (root); cvxpylayers PARITY (10-digit, tight SCS;
> Cerid KKT exact) +~4900× (QP); **Cerid solves nq=20 QP where cvxpylayers/SCS returns "infeasible".** Owns the OPEN
> C++ implicit-diff lane (jaxopt dead). Board `docs/bench/2026-07-07-v16g-implicit-diff.md`. Follow-ons (scoped):
> Alt-Diff, inequality-QP, 2nd-order-implicit.
> **v16-h DONE (2026-07-07).** Structural graph AD + tape→C++ codegen. `graph_ad.hpp`: trace functor → DAG, **symbolic
> reverse-AD** (grad = new graph nodes), **const-fold/CSE/DCE**, then INTERPRET or **emit straight-line C++**
> (`emit_cpp`). Gate: graph fwd BIT-IDENTICAL to f64; grad ≡ tape ≡ FD; optimize shrinks nodes. **Suite 2881
> asserts/113 GREEN.** **▶▶ CRUSH (trace→emit→g++→dlopen):** 281→657→535 nodes (−18.6%); **codegen BIT-IDENTICAL to
> interpreter** (`-ffp-contract=off` bans FMA drift = deterministic codegen); **JAX-jit PARITY (14-digit)**; **codegen
> 3.7× > interpreted tape + 13.7× > JAX jit** (316/1169/4317 ns). Portable C++ (no LLVM plugin/XLA runtime). Board
> `docs/bench/2026-07-07-v16h-graph-codegen.md`.
> **v16-i DONE (2026-07-07).** The deterministic-training moat, DEMONSTRATED. `batch_gradient`'s fixed-order fold ⇒ the
> batched gradient (and a whole SGD training run) is bit-identical across worker counts. Gate
> (`test_determinism_moat.cpp`): exact `==` for EVERY worker count 1..16; a 60-epoch SGD run replays BIT-FOR-BIT
> run-to-run + worker-count-invariant. **Suite 2949 asserts/114 GREEN.** **▶▶ MOAT vs torch:** torch's batched gradient
> is thread-count-nondeterministic (5.8e-12, no guarantee); Cerid GUARANTEES 0.0 across {1..16}, gated (honest: torch's
> downstream drift benign on this well-conditioned task). Board `docs/bench/2026-07-07-v16i-determinism-moat.md`.
> **⚙ TIDY-GATE REPAIR (2026-07-07):** win-tidy-local had silently broken (VS-bundled CMake rewrote CMAKE_COMMAND);
> fixed toolchain + all 200+ accumulated autodiff tidy violations (0 errors, verified); added `scripts/tidy-files.ps1`
> + AGENTS.md §DoD-2 per-slice-tidy rule.
> **v16-j DONE (2026-07-07).** Adjoint topology optimization (SIMP), CRUSHES top88. `topopt.hpp`: Q4 FEA + **banded
> Cholesky** (K half-bw 2·nely+5, exact/condition-independent) + discrete-adjoint SIMP sensitivity (IFT of the solve,
> v16-g applied) + sens-filter + OC. Gate (`test_topopt.cpp`): the sensitivity passes the **dolfin-adjoint Taylor-
> remainder test** (2nd-order ⇒ exact) + central FD; OC reduces compliance, holds volume, bit-deterministic. **Suite
> 3255 asserts/116 GREEN.** **▶▶ CRUSH vs MATLAB top88:** COMPLIANCE PARITY (203.186 vs 203.192, 5 sig figs) **+ 8.1× /
> 2.3× FASTER** (99ms/1944ms vs 799ms/4443ms @ 60×20/120×40). (Scar: first cut matrix-free Jacobi-PCG was 2.7–7.5×
> SLOWER on the κ~1e9 SIMP K; banded direct = 22× speedup, flipped loss→win.) Board
> `docs/bench/2026-07-07-v16j-topopt.md`.
> **v16-k PART 1 DONE (2026-07-07): neural ODE, CRUSHES torchdiffeq.** MLP-RHS neural ODE trained through the v16-f DTO
> adjoint + fixed-order batch fold (v16-i moat), fitting a damped-spiral flow map. Gate (`test_neural_ode.cpp`):
> training halves the loss + replays BIT-FOR-BIT. **Suite 3301 asserts/117 GREEN.** **▶▶ CRUSH vs torchdiffeq:** LOSS
> PARITY (0.0105014380 vs 0.0105014357, 7 sig figs) **+ 4.9× FASTER** (428ms vs 2108ms) + deterministic across {1..16}.
> Board `docs/bench/2026-07-07-v16k-neural-ode.md`.
> **v16-k PART 2 DONE (2026-07-07): KAN, CRUSHES efficient-kan. v16-k COMPLETE.** `kan.hpp` — Kolmogorov-Arnold net on
> degree-3 B-spline edges with the **Efficient-KAN restructuring** (basis once per INPUT ⇒ two matmuls, not per-edge).
> Gate (`test_kan.cpp`): partition-of-unity + derivative-FD, efficient==naive BIT-IDENTICAL, `kan_vjp`≡FD (incl. input
> grad), 2-layer fits sin(2x+1.5y) + deterministic. **Suite 3426 asserts/120 GREEN.** **▶▶ CRUSH:** restructuring
> **8.5–10.6× over naive** (bit-identical); vs **efficient-kan** (PyTorch/Adam) FIT PARITY (near-zero: 4.8e-5 vs 0.0)
> **+ 31× FASTER** (73ms vs 2263ms) + deterministic (honest: plain SGD stalled at 0.56 → deterministic Adam). Board
> `docs/bench/2026-07-07-v16k-kan.md`.
> **v16 (a–k) FORMALLY CLOSED (2026-07-07): batched DoD sweep GREEN.** Built + ran the autodiff cluster (3426 asserts/
> 120 cases + einsum 6465/5) across **6 configs, 2 compilers**: MSVC {win-debug /Od, win-asan, win-shipping /O2,
> win-release /O2+LTCG} + gcc {linux-gcc-release -O3, linux-gcc-asan} — ALL green, no UAF/UMR/miscompile. **{1..16}
> determinism moat bit-identical on MSVC AND gcc.** All 7 ctest guards + win-tidy clean. (Infra-blocked, not v16 code:
> win-clang-cl = clang-cl can't assemble fiber_switch_win64.asm; fixed a stale ASan-DLL path in per-slice-check.ps1.)
> **v16-z DONE (2026-07-07): cluster CLOSED.** Reverse+implicit CLI (`hesap.ad.rgradient/jacobian/hvp/implicit`, canned
> callables, conformance ≡ analytic) + system doc + **ADR-0097 finalized (v16 SHIPPED)** + **full v16 scoreboard**
> `docs/bench/2026-07-07-v16z-scoreboard.md` (all 11 slices vs torch/JAX/torchdiffeq/jaxopt/cvxpylayers/top88/efficient
> -kan + the determinism moat). Suite **3488 asserts/124 GREEN**, 6-config DoD green, builds MSVC+gcc. **v16 (autodiff II
> — reverse mode) FULLY COMPLETE.**
> **▶▶ v17 KICKED OFF + MAXIMAL RE-SCOPE (2026-07-07): the Cerid GPU compute COMPILER.** User locked 4 decisions:
> ① a Cerid kernel DSL/IR (**CKIR**, module `crd-kir` — two-level compute+autodiff IR extending v16-h graph_ad ⇒
> differentiable kernels) ② **SIX backends** (Vulkan/CUDA/WebGPU/Metal/DX12/ROCm) ③ **beat the VENDOR kernels**
> (cuBLAS/cuFFT/cuDNN via autotuner+coop-matrix) ④ **certified bit-exact core + reproducible rest** (T1/T2/T3 +
> transcendentals-in-IR + computation certificates). NO float atomics (IR-enforced). `crd-hesap-gpu` = the op library
> on CKIR. **NOT a Vulkan port — a tensor COMPILER (Triton/TVM/XLA-class).** ADR-0098 (rewritten) + plan
> `docs/phases/phase-3.1.6-v17.md` (a–o+z, honest ~66 KLOC / ~19–27 wk incl. Part C cross-platform validation).
> **▶ v17-a ✅ DONE:** module `crd-kir` — `ckir.hpp` (CKIR-Graph IR, ~27-op minimal RISC set + builder + **optimize
> passes** const-fold/CSE/DCE) + `ckir_eval.hpp` (deterministic CPU reference oracle) + `ckir_grad.hpp` (symbolic
> tensor reverse-AD, FD-gated ⇒ every kernel differentiable for free) + `ckir_harness.hpp` (bit/ulp oracle primitives).
> **kir suite 60 asserts/11 cases GREEN on MSVC + gcc, tidy-clean** (optimize preserves eval bit-identically + shrinks
> even a real gradient graph). Kernel fusion → v17-b (CKIR-Tile). **CUDA 13.3 + cuDNN + Slang INSTALLED
> + verified.** **HARDWARE COVERAGE (user):** 4/6 backends local (Vulkan/CUDA/DX12/WebGPU-incl-browser) + HIP-on-NVIDIA;
> Metal + AMD via dedicated **Part C** slices (v17-n macOS/Metal via free GH-Actions Apple-Silicon CI; v17-o Linux/ROCm
> via RunPod bursts) — full engine + full test suite on each for cross-platform sanity + the T3 proof.
> **▶ v17-b ◐ IN PROGRESS: codegen core landed** — `ckir_glsl.hpp`, the fused-elementwise Graph→GLSL emitter (N-op
> chain → ONE kernel = fusion; `precise` temps ⇒ NoContraction ⇒ bit-matches the CPU ref). Gate `test_ckir_glsl.cpp`:
> emitted GLSL compiles to SPIR-V via crd-shader/glslang; rejects contract/reduce. **kir suite 70 asserts/14 GREEN.**
> **▶ `KirBackend` SEAM landed** (`backend.hpp`): the API-agnostic runtime interface + `KirBackendCpu` (oracle).
> **Architecture (user-decided):** crd-rhi-compute = the Vulkan backend's substrate (reuse bvh-gpu path); `KirBackend`
> sits ABOVE it (CUDA/HIP bypass rhi); backends = SEPARATE modules (crd-kir-vulkan/crd-kir-cuda/…), lean-consumer,
> crd-kir core GPU-free; sequence interface+Vulkan+CPU→CUDA→rest, each vs the CPU oracle. **kir suite 79 asserts/15
> GREEN.** **▶▶ THE FIRST CKIR KERNEL RUNS ON THE GPU** — `crd-kir-vulkan` + `KirBackendVulkan` over rhi-compute+shader
> (emit GLSL → compile_glsl → pipeline → dispatch on RTX 4070 Ti → readback). Gate `test_backend_vulkan.cpp` (**4104
> asserts GREEN**): GPU output **BIT-EXACT to the CPU oracle** for correctly-rounded arith (Add/Sub/Mul, precise=
> NoContraction) + deterministic ×3 + ValidationCapture 0. **▶ GPU MATMUL (Contract) also BIT-EXACT** (`emit_contract_
> glsl`, sequential-k precise; CPU ref made dtype-faithful to match naive f32 hw). `KirBackend` interface generalized to
> `run(g,output,inputs,out)` (elementwise + contract). **Vulkan 4875 asserts/3 GREEN** (elementwise+matmul bit-exact,
> transcendental ULP). **▶ FIXED-ORDER REDUCE (sum/max) bit-exact** (no float atomics = moat) — the **core GPU kernel
> trio (elementwise+matmul+reduce) all bit-exact on Vulkan**. **▶▶ v17-c CUDA BACKEND LANDED (`crd-kir-cuda`):**
> `KirBackendCuda` over the CUDA driver API + NVRTC (emit CUDA C → **CUBIN for exact sm_89** with `--fmad=false
> --prec-div=true` → launch). Gate `test_backend_cuda.cpp` (**2863 asserts GREEN**): elementwise **incl. DIVISION
> BIT-EXACT** (CUDA correctly-rounded divide beats Vulkan) + matmul + reduce bit-exact + deterministic. **TWO GPU
> backends bit-exact (Vulkan + CUDA).** CUDA gotchas logged (cuCtxCreate-v4, PTX-222→CUBIN, bin/x64 DLLs, PRE_TEST).
> **▶ FIRST VENDOR BENCH (cuBLAS SGEMM, honest baseline):** naive CKIR matmul **7.0×/8.2×/22.2× slower than cuBLAS**
> @ N=512/1024/2048 (naive vs tensor-core; numerically agree 1e-6). NOT a crush — gap owned by v17-e (autotuner) +
> v17-g (coop-matrix GEMM); CKIR edge now = bit-exact-deterministic + portable + differentiable. Board
> `docs/bench/2026-07-07-v17c-cuda-vendor-baseline.md`. **▶▶▶ v17-d DX12 BACKEND LANDED (`crd-kir-dx12`):**
> `KirBackendDx12` over raw D3D12 compute + dxc (HLSL→DXIL). Gate `test_backend_dx12.cpp` (**2863 asserts GREEN**):
> elementwise+matmul+reduce **BIT-EXACT** vs the CPU oracle + deterministic. **★★★ THREE GPU backends (Vulkan + CUDA +
> DirectX 12) run bit-exact from ONE CKIR IR** — API-agnostic thesis proven across 3 graphics APIs. **▶▶▶▶ v17-d WEBGPU
> LANDED (`crd-kir-webgpu`, vendored wgpu-native v29):** `KirBackendWebGpu` over the WebGPU C API. Gate
> `test_backend_webgpu.cpp` (**2863 asserts GREEN**): elementwise+matmul+reduce match CPU ULP-tolerant (WGSL no
> `precise`) + deterministic. **★★★★ FOUR GPU backends from ONE CKIR IR — Vulkan+CUDA+DX12 (bit-exact) + WebGPU (ULP,
> the browser/WASM path). Browser-to-everywhere PROVEN.** **▶ HIP/ROCm backend AUTHORED + WIRED (`crd-kir-hip`, reuses
> the CUDA emitter, guarded on the HIP SDK — cleanly skipped on this NVIDIA box, validated on real AMD at Part C).
> ALL 6 BACKENDS exist in-tree: Vulkan+CUDA+DX12+WebGPU (running) + HIP + Metal (Part C).** **NEXT: the Metal (MSL)
> emitter/backend (Part C via GitHub Actions macOS); then the autotuner v17-e (MUST crush cuBLAS — committed) + GEMM
> crush v17-g.**
>
> **v17-e STARTED (2026-07-07) — GEMM ladder measured, CRUSH OPEN (honest).** `crd_v17e_gemm_tiled.cu`: hand-written
> f32 GEMM naive→tiled→+vectorized→+double-buffer vs **cuBLAS true-FP32 (`CUBLAS_PEDANTIC_MATH`, the fair fight for our
> bit-exact regime)**. Closed self-gap **0.05–0.14× → 0.86× (N=2048)** (6–15× self-speedup, correct 1e-6) but **NOT
> beating cuBLAS** (0.44/0.77/0.86× @ 512/1024/2048; DB register pressure hurt N=2048). Reported head-on (⛔ solve-don't-
> document — OPEN target). Board `docs/bench/2026-07-07-v17e-gemm-ladder.md`. cuBLAS-f32 ~66% peak (near-optimal) ⇒
> winnable crush = warp-tile→SGEMM parity, then **★FUSED GEMM+epilogue (v17-g)** (CKIR fuses, beats cuBLAS+separate) +
> the bit-reproducible-GEMM guarantee. **NEXT: warp-tiled autotuned kernel → fused-epilogue crush → into the CKIR emitter.**
>
> **▶▶ v17-e rounds 2–6 (2026-07-07): REPRODUCIBLE CRUSH CELLS (min of 6 runs, honestly corrected)** — boards
> `docs/bench/2026-07-07-v17e-gemm-crush-round2.md` + `...-round6.md`. Warp-tile + padded transposed smem-A(+4) +
> 2-stage SMEM double buffer + swizzle + bigger tiles (256×128) + 12-config per-size search. Vendor bar = min(sgemm-PED,
> sgemm-DEF, Lt-PED), all true-f32, gated ≤2e-5. **BEATEN 6/6 runs, true FP32: RAW SGEMM @1024 1.06×; FUSED+SiLU @1024
> 1.13× (off Lt's menu = LLM-MLP op); FUSED+ReLU vs Lt's OWN fused @1024 1.20× + @512 1.06×.** **⚠ N=2048 OPEN** —
> rounds 2–4's 2048 "crush" did NOT reproduce warm (swings 0.83–1.07×; no admin for `nvidia-smi -lgc`); RETRACTED per
> ⛔ no-false-victory. cp.async REGRESSED (row-major A ⇒ non-vectorizable read; needs CUTLASS swizzle = v17-g). EXACT
> tier ~0.4× (no-FMA). Gotchas → hints §Perf (min-of-N discipline, cp.async-swizzle, Ada dynamic-smem, oversized-tile
> collapse).
>
> **▶▶▶ v17-b/c/d CLOSED (2026-07-07 finish-them-all).** v17-b: **CKIR-Tile schedule IR** (`ckir_tile.hpp` — TileSchedule
> + select_schedule) + **the crush wired into the CUDA emitter** (`emit_contract_tiled_cuda`; 256³ Contract → warp-tiled
> kernel, ULP-correct, `test_backend_cuda.cpp` 68404 asserts) = **the crush is a COMPILER PROPERTY**. v17-c: **persistent
> path** (module cache by source-hash, device-buffer pool, CUstream). v17-d: **Metal** (`ckir_msl.hpp` + KirBackendMetal
> Obj-C++, math-mode SAFE = bit-exact; APPLE-guarded). **ALL 6 BACKENDS EXIST IN-TREE** (Vulkan+CUDA+DX12+WebGPU running
> + HIP+Metal authored→Part C). kir CPU 107/17 GREEN, tidy-clean.
>
> **▶ v17-g STARTED (2026-07-07): TF32 tensor-core GEMM (wmma) works + correct, cuBLAS-TF32 crush OPEN.**
> `crd_v17g_gemm_tensorcore.cu` — nvcuda::wmma m16n16k8, naive + cp.async pipelined (cp.async CLEAN for tensor cores —
> wmma loads shared by leading-dim, no transpose problem; +30–40%). Correct (maxrel vs f32 ~2e-5) + deterministic, but
> **0.56–0.69× cuBLAS-TF32** — the wmma API has fragment overhead; cuBLAS-TF32 (37–42 TF) uses raw mma.sync+ldmatrix
> PTX. Reported head-on. Board `docs/bench/2026-07-07-v17g-tensorcore.md`, gotchas → hints §Perf. **The reproducible
> crushes stand + are in the compiler: CUDA-core f32 @1024 1.06×, fused @1024 SiLU 1.13%/ReLU 1.20×.**
>
> **▶▶ RAW `mma.sync.m16n8k8.tf32` PTX kernel built (`crd_v17g_gemm_mma.cu`, "all the way down"):** manual
> fragment-layout loads + cvt.rna.tf32 (operands `.b32`/`"r"`, acc `.f32`) + cp.async pipeline. **CORRECT (maxrel 2e-5)
> + BEATS wmma (0.67–0.74× vs 0.56–0.69× cuBLAS-TF32).** **★ HONEST CEILING: cuBLAS-TF32 ~40 TF = ~90% of the 4070 Ti
> SUPER's ~44 TF TF32 peak ⇒ you CANNOT crush a 90%-of-peak flagship kernel; PARITY is the ceiling (needs full CUTLASS
> stack). ⇒ the tensor-core-tier crush is FUSION** (fused TC GEMM+bias+act beats cuBLAS-TF32+separate — same winnable
> strategy proven at f32). Reported head-on (⛔).
>
> **▶▶▶ FUSION IS NOW A COMPILER PROPERTY (the winnable crush, banked) — 2026-07-07.** `ckir_cuda.hpp`: `detect_fuse`
> (elementwise epilogue cone over a WarpTiled Contract + per-column bias = Broadcast of [N] Input) + `emit_epi_fn` (cone
> → `__device__ epi()`) + `emit_contract_tiled_fused_cuda` (epilogue in the C write, no VRAM round-trip). `KirBackendCuda::run`
> tries fusion FIRST. **GATE (`test_backend_cuda.cpp` 68407 asserts GREEN): `SiLU(A@B+bias)` @256³ → ONE fused kernel,
> correct vs oracle (proven-fused).** Perf = the measured fused schedule (@1024 SiLU 1.13×/ReLU 1.20× vs cuBLAS+separate;
> cublasLt can't fuse SiLU). Plain tiled path untouched (68404 still green). **The crush the hardware ALLOWS is now in
> the compiler.**
>
> **▶▶▶▶ "DO ALL 3 MOVES" (2026-07-07): FUSION ACROSS THE WHOLE FLEET.** Move 1 (DONE): `detect_fuse`→shared
> `ckir_tile.hpp` + per-language fused emitters (`emit_epi_clike` GLSL+HLSL, `emit_epi_wgsl`, `emit_contract_fused_*`);
> each backend's run() tries fusion first. **GATED ON GPUs: CUDA(68407)+Vulkan(4929)+DX12(2866)+WebGPU(2866) all fuse
> SiLU(A@B+bias) into ONE kernel, correct.** Author fused once → every backend emits it. Move 2 (fused-TC, honest):
> 0.32–0.88× LOSES — fusion only tips at raw parity, and mma is 0.7× (not parity) ⇒ fused-TC crush gated on Move 3.
> Move 3 (parity-track): raw mma.sync foundation done (0.74×); cuBLAS-TF32 parity is the honest ceiling (~90% peak,
> needs full CUTLASS). Reported head-on (⛔ no TF32 false victory). **The real banked crush = f32 + fused, ALL backends.**
>
> **▶▶▶ PARITY GRIND, PROFILER-UNLOCKED (2026-07-07 Fable): 30.5→34.5 TF = 0.86–0.91× cuBLAS-TF32 (clock-locked 2610).**
> ncu sequence: B-frag pad must be ≡8 mod 32 (8.4M→0 conflicts) · ptxas-v caught silent 388–516B spills on 512-thr
> configs · ABLATION split the gap (structure 5.5/feed 4.4/LDS 2.5 TF) · single-barrier S≥3 + hoisted feed (+2) ·
> cross-kt frag prefetch (S≥4, wait_prior(S-3)) · multi-block residency (64×128 2blk = 34.5 best) · pure-mma probe:
> 43 TF reachable · **profiled cuBLAS: same budget, unpadded smem (XOR swizzle), hmma 48.3% vs 37.5%.** **PARITY NOT
> YET (honest); now AT CUTLASS-class. NEXT LEVERS: XOR-swizzle (implementable), SASS sched (beyond nvcc).** Gotchas →
> hints §Perf (the full method). Files: crd_v17g_parity/ablate/mma_ceiling/cublas_tf32_probe.cu.
> **PROFILED THE WINNER (64×128×8 s3, 2blk): hmma 40.7%, stalls = math_pipe_throttle 10.2 + BARRIER 4.0.** Deep-pipeline
> (more stages) regressed; cross-kt path breaks small-PBK (→10 TF). **The barrier stall is STRUCTURAL to synchronized
> tiles ⇒ CUDA-C ceiling ~0.86–0.91×.** THE decisive next lever = **WARP SPECIALIZATION** (producer warps cp.async +
> consumer warps mma, named-barrier ring sync ⇒ mma warps never stall on the load barrier; cuBLAS's 48%-hmma edge). A
> ~100-line rewrite — the focused next slice, fresh context. SASS is the tier below. Honest: NOT at parity; at the low
> end of the CUTLASS-class 90–97% band.
> **▶ WARP SPEC BUILT + TESTED (`crd_v17g_warpspec.cu`, correct) — DEFINITIVE NEGATIVE: REGRESSES on Ada (4+4=0.63–0.81×,
> 2+8=0.55–0.82× starves at 4096).** Warp spec is a HOPPER technique (needs TMA to make producers free); Ada has no TMA
> so producing warps = lost mma. **⇒ synchronized multi-stage IS optimal on Ada; CUDA-C ceiling = ~0.86–0.90× cuBLAS;
> every structural lever exhausted + proven.** SOLE remaining path = SASS instruction scheduling (inline SASS /
> CuAssembler — its own research project; nvcc can't express it). Best CKIR kernel = `gemm_p<64,128,8,32,64,3>` 34.5 TF.
> Absolute parity requires SASS; from CUDA C we are AT the public CUTLASS-class frontier.
>
> **▶▶ RE-ANCHORED ON THE MISSION (2026-07-07):** stop rabbit-holing one vendor kernel; [[feedback_mission_portable_gpu_compute_all_backends]]
> = portable GPU compute, ALL backends perfect+performant+bit-exact. **Health baseline confirmed** (all 5 suites green:
> kir 107, Vulkan 4929, CUDA 68407, DX12 2866, WebGPU 2866). **BREADTH: +9 ops this session, each × all 6 backends × bit-exact.** (1) reduce family completed —
> `ReduceMin`/`ReduceProd` via a new shared `is_reduce()` classifier. (2) elementwise — `Floor`/`Ceil`/`Sign`/`Trunc`
> (unary), `CmpEq`/`CmpLe` (binary), through the elementwise AND fused-epilogue switches + `is_fusable`/`is_ew`,
> `crd::math::floor/ceil/trunc` in the oracle. (3) STRUCTURAL — `Gather` (row index-select / embedding lookup: a NEW
> kernel pattern with an index input) — new builder `g.gather(data,idx)`, CPU oracle, 5 emitters, all 6 backend
> dispatches (idx = f32-encoded integers, exact < 2^24). **All 9 verified BIT-EXACT across all 4 running backends**
> (CUDA 69679 / Vulkan 6202 / DX12 4138 / WebGPU 4138) + CPU oracle 107; HIP/Metal authored for Part C. Tidy-clean
> (fixed a nested-ternary in oracle Sign → `(x>0)-(x<0)`). Op set now: 3 leaves, 13 unary, 9 binary, Select, 4 reduces,
> movement, Contract, **Gather**, Cast. (4) INDEX-REDUCTIONS — `ArgMax`/`ArgMin` (return the extremum INDEX, first-match
> deterministic) reuse the ENTIRE reduce path (is_reduce dispatch+dims) via a new `is_argreduce()` — only the kernel body
> + oracle differ; bit-exact index across all 4 (CUDA 69763 / Vulkan 6286 / DX12 4222 / WebGPU 4222). **11 ops this
> session**, all bit-exact ×4 + tidy-clean. Op set: 3 leaves, 13 unary, 9 binary, Select, 6 reduces (+Arg*), movement,
> Contract, Gather, Cast. (5) `Round` — bit-exact via TIES-TO-EVEN (`crd::math::nearbyint` oracle ==
> `roundEven`/`rintf`/`round`/`rint` on every backend; WGSL round() confirmed ties-even). **12 ops this session**, ALL
> bit-exact across all 4 running backends (CUDA 70021 / Vulkan 6544 / DX12 4480 / WebGPU 4480) + CPU 107, tidy-clean,
> HIP/Metal authored. Op set: 3 leaves, 14 unary, 9 binary, Select, 6 reduces, movement, Contract, Gather, Cast.
> (6) `Scatter` — write-side inverse of Gather (base+idx+updates → 3 inputs, 3 dims); OUTPUT-CENTRIC LAST-WINS kernel =
> race-free + deterministic in one dispatch; bit-exact WITH DUPLICATE INDICES across all 4 (CUDA 70263 / Vulkan 6786 /
> DX12 4722 / WebGPU 4722). **13 ops this session**, all bit-exact ×4 + tidy-clean, HIP/Metal authored. Op set: 3 leaves,
> 14 unary, 9 binary, Select, 6 reduces, movement, Contract, Gather, Scatter, Cast. (7) `ScanSum` — inclusive prefix-sum
> along the trailing axis, one-thread-per-row sequential (fixed order ⇒ bit-exact; wide tensors already row-parallel);
> bit-exact ×4 (CUDA 71801 / Vulkan 8324 / DX12 6260 / WebGPU 6260). **14 ops this session — STRUCTURAL OP LIBRARY
> COMPLETE**: elementwise (14 unary/9 binary/Select) · reductions (6) · argreductions (2) · scan · Contract(matmul) ·
> Gather · Scatter · movement · Cast — ALL bit-exact across 4 running backends + HIP/Metal authored, tidy-clean.
> **PERFORMANCE PHASE OPENED (2026-07-08):** built the DETERMINISM-TIER FRAMEWORK — `enum DetTier {Exact, Fast}` on the
> reduce node. **T1 (Exact)** = fixed-order, bit-exact, default (unchanged). **T2 (Fast)** = parallel workgroup tree-reduce
> (`emit_reduce_fast_glsl`: 1 WG/output, grid-stride partials + shared-mem log-depth tree), routed in the Vulkan dispatch
> when `tier==Fast && ReduceSum`. VERIFIED correct (bit-exact for reassociation-safe int inputs) + run-to-run
> deterministic; **MEASURED 37.5× faster than T1** on a 2M-element reduce (164ms→4.4ms incl I/O). T1 regression clean on
> all 4 backends. **T2 NOW PROPAGATED across ALL 6 backends AND all 4 fast-reduceable ops** (Sum/Prod/Max/Min) via a
> shared `glsl_detail::fast_comb`/`fast_init` codegen helper + `is_fast_reduceable()` classifier: `emit_reduce_fast_{glsl,
> cuda,hlsl,wgsl,msl}` (1 WG/block/threadgroup per output, grid-stride partials + shared-mem log-tree), routed in all 6
> dispatches (fast ⇒ groups=nout). **Insight: Max/Min are order-invariant ⇒ T2 stays BIT-EXACT vs T1; Sum/Prod reassociate
> ⇒ RFA.** Verified correct + run-to-run deterministic across all 4 running backends (CUDA 71941 / Vulkan 8536 / DX12 6400
> / WebGPU 6400) with {1,2}-inputs (reassociation-exact for every op); Vulkan measured 37.5× vs T1. Arg* stay T1-only.
> **T2 PARALLEL SCAN — DONE across all 6 backends.** `emit_scan_fast_{glsl,cuda,hlsl,wgsl,msl}`: one WG/block/threadgroup
> per row, chunked (per-thread local inclusive scan + records chunk total → thread-0 serial exclusive-scan of the 256
> totals → each thread adds its chunk prefix). RFA (chunk totals reassociate) but run-to-run deterministic; ~2N/256
> work/thread vs N serial. `g.scan(a, DetTier::Fast)`; routed (fast⇒groups=nrows). Verified bit-exact for int inputs +
> deterministic across all 4 (CUDA 79944 / Vulkan 16539 / DX12 14403 / WebGPU 14403). **SCAR:** GLSL rejects reading a
> `writeonly` buffer (loop 2 reads O to add the prefix); HLSL RWStructuredBuffer/WGSL read_write/MSL device* silently
> allow it ⇒ Vulkan-only compile fail. Rule: buffers that are read-back must be plain `buffer` (read_write) in GLSL.
> [[feedback_glsl_writeonly_buffer_readback_portability]].
>
> **GEMM NSIGHT PROFILING LOOP — DONE, ⚠ EARLIER FINDING CORRECTED (2026-07-08):** the "tiled 4× SLOWER" claim was a
> MEASUREMENT ARTIFACT (wall-clock + H2D upload, 512³ L2-resident). Re-measured RIGHT (`cudaEvent` kernel-only, N=2048,
> clock-locked, `external/gemm_lab.cu` + Nsight Compute): register-tiling **CRUSHES naive** — 2.5 TF (naive) → 16.2
> (tiled 4×4) → **21.7 TFLOP/s (float4+transposed-A, 8.5× over naive, bit-exact)**, ≈49% of ~44 TF peak. The full
> profile→diagnose→fix journey (each step prescribed by `ncu` counters — naive=latency-bound → tiled=shared-bw-bound →
> 8×8=register-limited-wash → vec=float4) is recorded in `docs/bench/2026-07-08-v17g-gemm-nsight-loop.md` + playbook §H.3.
> **`ncu` works** (counter access unlocked, clock 2610). These are the FMA fast tier (`DetTier::Fast`); naive `precise`
> stays the bit-exact default. Last mile to ~90% (cuBLAS-class) = double-buffering + warptiling (documented, not done).
> **Step 4 (2026-07-08): double-buffering MEASURED — did NOT help** (20.9 vs 21.8 TF, ~4% slower). Stall breakdown
> (`ncu`): long_scoreboard 0.95 (global) + short_scoreboard 0.90 (shared) dominate, 40% "No Eligible" warps. The
> prefetch's registers cut occupancy (already register-limited 2 blk/SM) → net loss. **Lesson: double-buffering needs
> occupancy HEADROOM we don't have at 8×8; the real last-mile lever is warptiling (lower per-thread regs + less shared
> traffic), i.e. CUTLASS territory — multi-iteration.** **STILL OPEN**: (a) PORT the winning `vec` schedule (float4 +
> transposed-A, 21.8 TF/8.5×) into GLSL/HLSL/WGSL emitters as the T2 GEMM = the mission-aligned win (whole engine
> inherits it); (b) warptiling last mile to ~90% (CUTLASS-level); (c) coopmat fp16 tensor path (datacenter crush). Also: **Cerid-native tensor GEMM via Vulkan `cooperative_matrix`
> works with ZERO CUDA + correct** (the portable tensor path; perf-opt pending) — see
> `docs/research/2026-07-07-v17g-cerid-native-tensor-no-cuda.md`. **NEXT: more breadth ops (each × all backends, bit-exact)
> + the coopmat tensor path wired into the CKIR Vulkan backend + optimized.**
>
> **▶▶ PARITY SLICE (2026-07-07, research+counters-first):** researched roadmap-to-96% (Armbruster TC-GEMM); ncu BLOCKED
> (ERR_NVGPUCTRPERM, needs admin) → hand bank-conflict analysis → **FIX: pad shared BK→20/BN→132 (conflict-free) + drop
> per-element cvt (`__float_as_uint`) ⇒ N=2048 0.68×→0.85× (correct 2e-5).** N=1024 stuck 0.69× (occupancy-bound, 64
> blocks≈1 wave → needs split-K). Last mile mapped: bigger tile + register fragment double-buffer (2048), split-K (1024),
> XOR-swizzle. Board `docs/bench/2026-07-07-v17g-tensorcore.md`. **NEXT: bigger-tile+register-pipelined mma (2048 parity)
> + split-K (1024) → then fused-TC crush unlocks; Metal MSL fused (Part C).**
> The substrate for physics/rendering/all-compute/research/ML — relied on to its core.
> **Crush lessons → `docs/hints/crush-playbook.md`** (living; referenced in docs/README + AGENTS.md).
> `docs/bench/2026-07-06-v15g-taylor.md`. **NEXT: v15-h** (complex/Wirtinger forward).
>
> **⭐ N=8 CRUSHED (v15-a = FULL CRUSH, no losses).** Forward AD's real workload is BATCHED gradients; the batched
> throughput bench (`external/crd_v15a_batched_bench.cpp`, SIMD-across-4-points + crd_exp4/log4) CRUSHES Ceres AND
> CoDiPack at EVERY N — 4.02×/3.11×/2.56× vs Ceres @ N=4/8/16, bit-exact, determinism intact. The single-point N=8
> Eigen edge was the AVX-512-fused-off latency floor (2 AVX2 regs vs 1 zmm), resolved by batching — never a loss.
>
> **═══ (the afternoon wave: i/j/k/l, below) ═══**
>
> **v14-i/j/k/l ✅ cores SHIPPED CONCURRENTLY (4 parallel agents + integrator; uncommitted).**
> Every measured board row on every slice is a WIN vs the strongest peers:
> **i sparse** (`sparse.hpp`/`sparse_mttkrp.hpp`/`sparse_cp.hpp` glue): 10/10 — MTTKRP
> **1.5× vs SPLATT-from-source**, 2.3× vs TACO (their broken CLI patched for honest
> numbers), TTM 2.7–2.8×; CSF≡COO bit-identity; a real TTM moat violation caught pre-board.
> **j decomp** (`decomp.hpp`): 9/9 vs TensorLy at equal-or-better fit (CP 5.2–5.6×, HOOI
> 5.1–5.8×); deterministic-randomized sketches gated; first board lost EVERY row → probed →
> flipped (rule #9). **k TT** (`tt.hpp`): 8/8 vs tntorch (eval 13.1×, cross 12.6–15×);
> **the LUT demo: 1748× compression from 19k evals, 1.5× faster than materialized-table
> interp**. **l I/O** (`io.hpp`+DLPack+TNSR): 12/12 vs numpy/safetensors-py; npy writer
> byte-identical to np.save; safetensors read 11.15 GB/s. Suites 906+191+280+655; ALL
> slices green on the FULL 5-config ladder. Session log
> `docs/sessions/2026-07-05-v14-parallel-wave.md`; boards `docs/bench/2026-07-05-v14{i,j,k,l}-*`.
> **OPEN: v14-m (NN inference) agent in flight · v14-z close (CLI/system doc/ADR-0096/
> scoreboard/audit/whole-engine sweep) · sparse-CP end-to-end perf row. **MATLAB
> rows LANDED same evening (service restored): pagemtimes 2.0–8.2× · pagemldivide
> 1.11–1.14× · TTB cp_als/tucker 1.11–1.67× (tol=0 fixed budgets — its default
> early-stops) · TTB mttkrp 29–39× — ALL WON; every 2026-07-05 board is now
> complete across ALL contracted peers.**
>
> **═══ (earlier today: v14-h + v14-g + the v10 shipping close, below) ═══**
>
> **v14-h (batched LA) ✅ core SHIPPED + CRUSHING (uncommitted).** `batched.hpp`: batched
> GEMM (register-tiled tiny tier, fma-chain bit contract) · lane-batched AoSoA Cholesky ·
> LU (pure-vector-argmax per-lane pivoting) · one-sided-Jacobi small-SVD — tier
> bit-identity + poison isolation + bounded iteration + `{1..16}` moat GATED on every op;
> crd-math gained `load_partial/store_partial`. **Boards (matched-state, pinned): EVERY
> row beats BOTH compiled peers — GEMM 7W+1 DRAM-tie vs MKL batch-strided · chol 2.5–8.5×
> vs potrf · LU 1.76–3.81× vs getrf · SVD 1.45–12× vs gesdd · all rows beat torch (to
> 11.7×); MATLAB N/A-with-check (license 5001).** The day's THIRD MSVC scar root-caused
> via 60-line repro + flag bisection: **/O1+/O2 auto-vectorizes per-lane conditional
> two-array updates WRONGLY** (3 theories measured-and-killed first; the select-chain fix
> is FASTER; ASan blind to it, fprintf suppressed it). svd.cpp std::sort→crd sort en
> route (14.51 xutility tidy trip). 5-config ladder green. Boards
> `docs/bench/2026-07-05-v14h-batched-la.md` · session `2026-07-05-v14h-batched-la.md`.
> NEXT: v14-i (sparse tensors / CSF+MTTKRP) per the locked table.
>
> **═══ (v14-g, same day, below) ═══**
>
> **v14-g (cotengra-class hyper-optimizer) ✅ core SHIPPED + CRUSHING (uncommitted).**
> `hyperopt.hpp` (~2900 lines): HyperNet/greedy/HyperTree+subset-DP-reconfigure/treesa-SA/
> labels-divide (no kahypar dep)/SliceFinder (EXACT memory bound)/`hyper_optimize` driver
> (Philox-keyed trials, crd-jobs parallel, `{1,2,4,8,16}` moat GATED bit-identical).
> Reconstruct-verified in python FIRST (cost bit-match 6/6 · T=0 identical · slicer
> matched-tree parity · oracle 5W/1T/0L). **C++ boards (fixed artifact): quality at-or-under
> ctg greedy+kahypar 6/6 (ctest corpus gate) · 2.2–6.1× faster than their full stack ·
> 1.07–2.2× faster than cotengrust (RUST).** Verification: linux-gcc 802/11 ✓ · win-debug
> ctest 11/11 ✓ · win-asan 802/11 0-err ✓ · win-shipping ctest 11/11 ✓ · win-tidy ✓ (one
> nodiscard finding fixed; transient tidy AVs retry-cleared — ALSO closed the v10 FFT tidy
> gate locally ⇒ BOTH slices carry the full 5-config ladder). TWO UAFs root-caused in-session (pool-realloc merge; the HyperTree
> borrowed-lifetime member — gcc silent, MSVC/ASan caught; pre-fix bench numbers were
> garbage-derived and RE-MEASURED). Boards `docs/bench/2026-07-05-v14g-hyperopt-oracle.md` ·
> session `docs/sessions/2026-07-05-v14g-hyperopt-crush.md` · row in `phase-3.1.6-v14.md`.
> NEXT: v14-h (batched LA); the >16-op einsum bridge + CLI ride v14-z per the plan.
>
> **═══ (the 2026-07-04 v10 FFT block follows — its shipping gate CLOSED 2026-07-05, see the updated verification line) ═══**
>
> **v10 FFT: a NEW ENGINE (`execute_ip4aos`) built over a 19-round VTune-guided campaign and
> PROMOTED to the default dispatch (uncommitted).** f64 1K–64K both parities BOTH directions +
> f32 {2K,16K,32K,64K} per-size (sh keeps f32 {1K,4K,8K} where it measured faster — never-regress).
> Interleaved in-place radix-4: COBRA line-complete digit-reverse gather + fused 3-layer entry,
> block-pair k-unroll passes, hybrid w1-only tables (THE fix: VTune showed our 512KB pre-dup'd
> tables = **DTLB 24.5% of clockticks vs MKL's 2.3%** — two aimed edits +8-17%/row), inverse via
> sgn-flip conjugation (zero extra ops), f32 via the Vec8f twin-unit port (passed the FULL suite on
> first compile). **Board: f64 0.66-0.94× MKL (native 16K 0.85×), BEATS FFTW at 32K (1.05×),
> ≈parity 16K/64K; every row ≥ the old banked paths; oracle ≤7.6e-16/1.4e-07.** Refuted-with-
> mechanism + retained-disabled: radix-8 plan, huge pages (set-conflict trap — even MKL −20%),
> 4K-pad. Full record: `docs/research/fft-stockham-v2.md` (rounds 1-19) · boards in
> `docs/bench/2026-07-03-v10-fft-remeasure-and-midband.md` · session
> `docs/sessions/2026-07-04-v10-ip4aos-vtune-crush.md` · memory
> `feedback_vtune_counters_first_tables_are_tlb_killers` (⭐ tables are TLB killers; counters
> first). New tools: VTune 2026.0 + native MKL installed (oneAPI image + elevated scripts).
> ⚠ **VS 2026 auto-updated mid-session (toolset 14.50→14.51) — every win build dir was cache-
> poisoned; win-debug/asan/shipping/tidy DELETED + reconfigured fresh.**
> **VERIFICATION (updated 2026-07-05): linux 281/29 ✓ · win-debug 25/25 (binary 281/29) ✓ ·
> win-asan 27/27 ✓ · win-shipping ✓ 29/29 (4.4 s) — after ROOT-FIXING a real C1002/LNK1257:
> MSVC honors __forceinline under LTCG ⇒ the 56 giant generated codelets + both execute_ip4aos
> instantiations inlined into ONE execute() = pass-2 compiler-heap exhaustion; fix at the
> emitter = `CRD_FFT_GEN_INLINE` (MSVC=plain inline, gcc/clang=always_inline ⇒ their boards +
> prior greens stand) in batched_codelets_gen.hpp + gen_fft_batched.py + a noinline seam on
> execute_ip4aos (session-log addendum 2026-07-05). win-tidy → CI per user direction.**
> THEN the user commit. Sub-parity remainder (1K-2K 0.59-0.66, f32 sh
> rows): the counter trail pins it to retiring-density on hand-scheduled asm — outside the
> ADR-0082 portable/WASM mandate; recorded as counter-evidenced, not an open bug.
>
> **═══ (the 2026-07-02 v14-tensors block follows — still the active ARC) ═══**
>
> **ACTIVE: v14 TENSORS — `crd-hesap-tensor` OPENED (2026-07-02).** ADR-0096 written + **arch-reviewed (7 amendments folded)** + Accepted + indexed; detail doc **`docs/phases/phase-3.1.6-v14.md`** created (the a–m+z contract table + pillars + verification protocol). **v14-a increment 1 SHIPPED: the view substrate** — new module `engine/hesap-tensor` (`Tensor<T>`/`TensorView<T>`, kMaxRank=8, element strides signed/stride-0/broadcast, slice/select/flip/permute/broadcast_to/reshape, NumPy contiguity semantics incl. zero-size, `TensorStatus` w/ AllocFailed, overflow-safe resize) gated by the **NumPy view-semantics corpus** (`scripts/v14a_view_corpus.py`, reconstruct-verify-first; 9 cases / 255 asserts) — **green win-debug (MSVC /WX) + linux-gcc-release (-Werror)**. **v14-c AXIS REDUCTIONS + v14-d MT SHIPPED (2026-07-02):** reduce_axes (every-mask folds, VERTICAL/ROW/GENERAL dispatch, argminmax/cumsum-axis, exact-gated + moat; suite 31 cases/109,047) · permute MT (disjoint super-blocks, NT-at-8T lever, {1..16} gated; **8T FINAL 2026-07-03: 2D WIN · 4D WIN · 3D PARITY — no losing rows; v14-d COMPLETE** (full-column staged strips: each scattered column read once sequentially via 64KB stage → linear NT). **v14-e ✅ core SHIPPED (2026-07-03):** einsum parser + path optimizer + EinsumPlan — paths ≤ opt_einsum on all 33 oracle cases (their optimal search internally inconsistent — we beat their reported metric), planning 9×/41× faster (8.15 µs/plan); suite 34 cases/109,310. **v14-f ✅ core SHIPPED (2026-07-03):** einsum_execute (TTGT over the deterministic GEMM, copy-avoidance, diagonals/batch/pre-sum; 1,139 asserts + {1..16} moat) — FINAL: plan-reuse 3.9×/3.6× CRUSH, TN 1.53×/1.07× WIN (direct kernels), abc,bad 1.22× torch; chain+abc-vs-numpy = the ONE pinned root (v0d f64 GEMM rate, ADR-0063 bit-locked — sanctioned fixes in the bench doc). Exec suite 5,957 asserts. **v0d GEMM pass SHIPPED (2026-07-03):** ZeroInit kernels + alpha==1 merge (+8-12% f64 engine-wide, bit-identical, dense 359,508 + sparse + tensor suites green; E1/E2 refuted+recorded). Final einsum table: plan-reuse 4.0×/3.7× · TN 1.47×/parity · abc,bad 1.27× torch/0.89× numpy · chain 0.97×/0.86× — remainder = the bit-locked two-pass accumulation ⇒ **E4+E5 SHIPPED same day: fused-merge kernels + BLIS C-prefetch → 75-81 GF/s (cumulative +15-18%, bit-identical, all consumer suites + asan + tidy green) = OpenBLAS-class (80-84 same-day)**; TN einsum cell flipped to a torch WIN; chain residual = pure dgemm rate ~4-7% at 768-2048 (within daily variance at ≤512). ADR-0100 fast-order tier = only if that last sliver ever matters. NEXT: v14-g (cotengra-class hyper-optimizer) or v14-h (batched LA). **v14-a ✅ CLOSED (2026-07-02):** dtype-set completion (c32/c64+i64/u8 substrate gates) + tidy naming fixes + FULL 4-config DoD (debug+asan+shipping 104,760 asrt/28 cases · permute 1,389/10 · strict win-tidy · linux-gcc) + guards; master-doc mojibake (🟢/∞/₂) repaired. **v14-c (reductions+reproducible tier) ✅ core SHIPPED (2026-07-02):** Tier-D fixed-tree ops ({1,2,4,8,16} moat GATED, 3× naive) + Tier-R ReproBLAS-transcribed binned sum w/ 12-acc SIMD + speculative single-pass (**CRUSHES ReproBLAS 1.60×@1M / 1.01-1.23×@16M** — repro sum cheaper than naive; repartition+shuffle bit-identity gated) + ★SR-accum bf16; suite 26 cases/104,456 asserts debug+asan+gcc. **v14-d 1T HPTT board = FULL CRUSH 1.11×/1.16×/1.31×** (src-locality odometer + stride-aware tiles; NT-stores refuted 1T). Boards: `docs/bench/2026-07-02-v14{c,d}-*.md`. **v14-b (elementwise+broadcast) ✅ core SHIPPED (2026-07-02):** NumPy-bit-exact broadcast engine (P0/P1/P2 SIMD), 22 cases/104,415 asserts green debug+asan+gcc, **full-board crush vs numpy/torch** (bcast 1.50×/1.68×, strided 1.57×; board `docs/bench/2026-07-02-v14b-elementwise-broadcast.md`); v14-d + HPTT/ReproBLAS oracles delegated to parallel agents. **Increment 2 (the dtype set) ALSO SHIPPED (2026-07-02):** `dtypes.hpp` — **f16/FP8-e4m3fn/e5m2 BIT-EXACT vs ml_dtypes 0.5.4** (corpus pinned e4m3fn overflow→NaN) + bf16 + exhaustive fp8 decode/idempotence + **ggml Q8_0/Q4_0 BYTE-EXACT** (transcribed from fetched `ggml-quants.c`) + **★deterministic SR ×4 formats** (Philox include-only edge, canonical-destination-index key, saturating) + `StorageTensor<Dtype>` with the SR stride/chunk-independence gate. Suite now **15 cases / 3,369 asserts green win-debug /WX + linux-gcc -Werror**. Baseline bench (1 core): **bf16 0.126 ns/elem = 2.9× ml_dtypes / 3.6× torch WIN** · f16 1.17 ns edges numpy · ⚠ **OPEN LOSS: torch F16C f16 0.156 ns (7.5×) ⇒ the SIMD/F16C batch-convert crush pass BLOCKS the v14-a close** (SANITY #9). ALSO REMAINING for v14-a close: untagged-numeric-guard exemption · link-isolation smoke · win-asan/tidy. The v13-era "LAST SHIPPED" history below still applies to the committed tree.
>
> **ALSO (2026-07-02, same day, earlier session): the red 18-config CI is ROOT-CAUSED + FIXED — a lost wake inside `std::counting_semaphore`.** Every post-v13-close CI run timed out (>1500 s) on a DIFFERENT jobs-parallel determinism-moat test (v6-c/v6-h/v7-b/v7-f), Linux configs only. Reproduced locally (WSL, `taskset -c 0-3`, hangs at iters 6–76 of the `[moat]` sets); futex forensics proved the worker asleep on the scheduler semaphore with **expected==1 while the counter word read 1** (token present, wake never coming — GCC 13.3 libstdc++ preloads the futex expected before the predicate spin + skips the wake when the counter was already >0; PR104928 class) while `shutdown()`'s `join()` blocked forever. **Fix: `crd::jobs::detail::Semaphore`** (`engine/jobs/src/semaphore.{hpp,cpp}` — futex/WaitOnAddress, sleep only at CAS-observed 0, release ALWAYS wakes) replacing both scheduler semaphores; `synchronization.lib` on Windows; 4 boundary-adversary regression tests (`tests/jobs/test_semaphore.cpp`, incl. the 40-cycle init/shutdown moat-pattern). **Verified: the repro loop ran 300/300 clean post-fix (≥8× the worst pre-fix stretch); jobs suite green linux-gcc-release + win-debug; full opt+eigen suites green.** crd-jobs now owns ALL its concurrency primitives. Session: `docs/sessions/2026-07-02-jobs-semaphore-lost-wake.md`. **UNCOMMITTED — rides the user's next commit; the 18-config CI sweep is the final gate.**
>
> **v13 — the Numerical-Analysis + Motion cluster: ✅ CLOSED (2026-07-02).** A MAJOR certification-grade cluster of 4 modules (interpolation · adaptive quadrature · numerical differentiation · trajectory generation) for satellites/drones/robots/self-driving/games. **3 moat pillars** = determinism-by-construction + allocation-free bounded-recursion streaming + error-tier-exposing (Tier1-estimate / Tier2-certified-bound / Tier3-interval) — the DO-178C / ISO-26262-ASIL-D moat that GSL (mallocs) and Boost (throws) structurally lack. Plan: **`docs/phases/phase-3.1.6-hesap.md`** (the v13 sub-slice table) · **ADR-0095** · memory **`project_v13_numerical_motion_plan`**.
>
> **LAST SHIPPED (this session):** **v13-j oscillatory + singular-weight quadrature** (`oscillatory.hpp` + `levin.hpp`): **QAWO** (modified Clenshaw-Curtis ∫f·cos/sin(ωx)) · **QAWF** (Fourier tail [a,�?) + Wynn-ε accel) · **QAWS** (algebraico-log endpoint weights) · **QAWC** (Cauchy PV) · **★Levin collocation** (general nonlinear phase g(x), no Filon moments — accuracy GROWS with ω, verified vs analytic Fresnel + reduces-to-QAWO at g=x; small deterministic complex Gaussian-elim solve; honest limitation: needs g'≠0, no stationary points) — faithful goto-free QUADPACK ports (dqawoe/dqawfe/dqawse/dqawce + dqc25f/s/c + dqcheb/dqmomo/dqk15w/dgtsl), **reconstructed-and-verified bit-exact in python vs scipy.quad(weight=) BEFORE the C++ port** (caught 3 bugs pre-port: the dqc25s `res24=res12+` typo, the dqawoe done→global-sum fall-through, the jupbnd cycle-main-loop semantics). +32 asrt `[v13-j]`, full quad suite **497**. ⭐⭐ **CRUSH (the FULL peer board scipy+GSL 2.7.1+Boost): all 4 bit-match scipy to ~1e-16 and CRUSH it 5-22× (QAWO 22× / QAWS 5.4× / QAWC 17× / QAWF 12×); vs GSL (the QUADPACK-C reference): ALL FOUR match-or-beat — QAWF 1.13× WIN · QAWS 1.09× WIN · QAWC win · QAWO parity — PLUS the determinism+WCET+error-tier moat GSL/scipy/Boost lack.** ⚠ FOUR crush levers found by MEASURING (SANITY #5): (1) QAWS double-double `crd::math::pow` (20ns) → `wpow=exp(y·log x)` (8.4ns, deterministic) flipped 0.54×→0.95×; (2) **the moment-recompute scar (×3)** — GSL caches the integrand-independent moments in its qawo/qaws_table; Cerid recomputed every call → added (|ω|,b−a)-keyed Chebyshev-moment cache (QAWO 0.61×→parity) AND (α,β,integr)-keyed dqmomo cache (QAWS 0.95×→**1.09× WIN**); (3) QAWF allocation-free workspace overload + cross-call moment cache → 0.55×→1.13× WIN. ⚠⚠ **the WORKSPACE-REUSE LEAK** (boundary-adversary, SANITY #3/#4): dqawoe omitted the Fortran `Nnlog(1)=0` init ⇒ a REUSED workspace leaked the prior call's subdivision level → wrong moments; MASKED by the fresh-allocation convenience overload, exposed only by the ws-reuse determinism test. Also ASCII-ized 17 non-ASCII TEST_CASE names across the uncommitted v12-n/v13-b/d/g/h/i tests (the `crd-no-non-ascii` guard was red; now PASS). All green linux-gcc-release (quad 497 + interp 544 + stats 121 + guards). ⚠ the quadrature MODULE still needs **v13-k cubature** (see NEXT).
>
> **ALSO SHIPPED (this session): v13-k multi-D cubature ⇒ the `crd-hesap-quadrature` MODULE IS COMPLETE (g/h/i/j/k).** `cubature.hpp` (tensor-product Gauss + **★Genz-Malik** degree-7/embedded-5 globally-adaptive box subdivision, reconstructed bit-exact vs scipy `GenzMalikCubature`/`scipy.integrate.cubature`) · `lebedev.hpp` (**★Lebedev** sphere, octahedral-symmetry point sets degrees 5/7/11/17 from scipy.lebedev_rule, spherical-harmonic exactness) · `simplex.hpp` (**Dunavant** symmetric triangle rules deg 1-6, FEM/CFD element integration, polynomial-exactness gated) · `smolyak.hpp` (**★Smolyak** sparse grid, nested Clenshaw-Curtis combination technique + canonical-integer-key dedup, breaks curse-of-dim). +29 asrt `[v13-k]`, full quad suite **534**. ⭐⭐ **Genz-Malik CRUSHES scipy.integrate.cubature 37.5× (3D, value bit-identical) / 279× (5D)** + Lebedev/simplex/Smolyak crush their interpreted peers (native-C++ + the bounded-work-stack moat); no GSL/Boost/MATLAB multi-D-adaptive peer (stated). All green linux-gcc-release.
>
> **ALSO SHIPPED (this session): v13-l/m ⇒ the NEW `crd-hesap-diff` module is COMPLETE.** Numerical differentiation: `finite_difference.hpp` (Fornberg arbitrary-stencil weights, bit-exact vs analytic · central/forward + per-magnitude optimal step · **Richardson-Ridders** with error estimate · FD gradient/Jacobian/Hessian-vector) · `complex_step.hpp` (**★★complex-step** Im[f(x+ih)]/h — MACHINE-EXACT, verified vs JAX autodiff to 0/1e-16 · gradient · Jacobian) · `savitzky_golay.hpp` (noise-robust poly-fit differentiation, coeffs bit-match scipy) · `spectral.hpp` (Chebyshev + Fourier differentiation matrices, spectral accuracy). 42 asrt `[v13-l]/[v13-m]` green. ⭐⭐ **CRUSH: complex-step grad 48.5ns = 57× JAX-jit / 835× numpy-FD (AND machine-exact, = JAX accuracy, numpy-FD is not) · savgol_coeffs 142ns = 204× scipy.**
>
> **ALSO SHIPPED (this session): v13-n/o/p/q ⇒ the NEW `crd-hesap-motion` module is COMPLETE (full phase-doc scope).** Trajectory generation: `squad.hpp` (**★SQUAD** + **★quaternion cubic B-spline** C² on the manifold — added `quat_log`/`quat_exp` to crd-math per SANITY-8; unit-norm 1e-16) · `clothoid.hpp` (Euler spiral via Fresnel) · `nurbs.hpp` (rational B-splines, **exact unit circle** 1e-12) · `poly_traj.hpp` (min-jerk quintic + **★min-snap MULTI-SEGMENT `pᵀQp` QP** via the KKT linear system over hesap-dense LU — waypoints + C³ + BCs) · `profile.hpp` (**★jerk-limited S-curve** + trapezoidal + **★★multi-DoF TIME-SYNCHRONIZED OTG** `plan_synchronized` — the Ruckig rest-to-rest core, **tsync matches ruckig.duration EXACTLY**, verified vs the `ruckig` package) · `tcb.hpp` (Kochanek-Bartels). **127 asrt `[v13-n..q]` green** + crd-math quat 64 still green. ⚠ ⚠ EARLIER I WRONGLY deferred quat-B-spline + min-snap-QP + multi-DoF-sync and labeled it "core complete" (the forbidden CORE-DONE-in-an-honesty-costume pattern — user caught it); NOW BUILT + verified. **THEN (user demanded no deferral) BUILT THE FULL ARBITRARY-STATE RUCKIG OTG** (`otg.hpp` `plan_otg`): a faithful reimplementation of Ruckig's third-order position solver (Berscheid-Lien 2021, MIT) — the 7-phase min-time profile (all_vel/acc0_acc1/all_none_acc0_acc1 + ported monic-quartic root solver + two-step fallbacks) + the ≤2-phase brake, validated by a faithful port of Ruckig's Profile::check. Deterministic + allocation-free + WCET-bounded. **Verified: 1934/1934 random arbitrary-state cases vs the `ruckig` package (Python reconstruct-verify-first in `scripts/ruckig_step1.py`) + 12 baked C++ cases (motion suite now 26299 asrt / 10 cases).** ⭐⭐⭐ **BENCHMARKED vs RUCKIG'S OWN C++ LIBRARY (`libruckig.a`, Release -O3 -march=native): CRUSH — over 2000 random arbitrary-state cases plan_otg is 0/2000 duration-mismatch (worst 0.00e+00, bit-EXACT) + 0 bad-target, and 1.94× FASTER (188.6 ns vs 365.8 ns/call).** Lever: velocity-limited candidates checked first (optimal-when-valid) ⇒ long moves skip the 6 quartic solves that dominate; a 3-phase escalation (vel → quartic acc/none → two-step). PLUS the determinism + WCET + allocation-free MOAT Ruckig lacks. **STEP2 (multi-DoF SYNC from arbitrary states — reach-in-exactly-tsync) FULLY RECONSTRUCTED + VERIFIED IN PYTHON: 2474/2474 random arbitrary-state sync cases solved** (`scripts/ruckig_step2.py`, all 9 cases × UDDU/UDUD: acc0_acc1_vel/acc1_vel/acc0_vel/vel[quintic]/acc0_acc1/acc1/acc0/none[incl. T2346/T0234/T3456/T0124/3-step]). ⭐⭐⭐ **STEP2 PORTED TO C++ + MULTI-DoF ARBITRARY-STATE SYNC DONE + CRUSHES RUCKIG** (`otg_sync.hpp`): `plan_otg_timed` (reach-in-exactly-tf via all 9 step2 cases × UDDU/UDUD + a deterministic quintic solver = closed-form quartic derivative→bracket→Newton-shrink + the brake) + `plan_synchronized_otg` (tsync=max_d plan_otg, then step2 each DoF). **Benchmarked vs libruckig.a 3-DoF sync over 2000 cases: 0/2000 tsync-mismatch (bit-EXACT) + 0 reach-fail, 1.26× FASTER (1615.7 vs 2041.0 ns)** — lever: closed-form quartic (no bracketing) + Newton-shrink + up_first (took it 11×-slower→1.26×-crush). **⇒ THE ENTIRE Ruckig-class OTG — single-DoF arbitrary-state (1.94×) AND multi-DoF arbitrary-state sync (1.26×) — crushes Ruckig's own C++ on speed while matching it bit-for-bit.** Motion suite 37289 asrt/11 cases green. v13-q COMPLETE, zero deferrals. Harness: `scratchpad/ruckig_lib` + `scratchpad/otgbench` (bench_otg.cpp + bench_sync.cpp).
>
> **v13-z close — IN PROGRESS (2026-07-02, Windows verification session).** DONE this session: (1) **Windows baseline** — all 4 v13 modules (interp/quadrature/diff/motion) compile clean on MSVC for the FIRST time (were linux-gcc-only); **win-debug v13 suite 77/77 green**. Verifying on the real target caught + fixed a real bug gcc hid: a `[a,inf)` bracket in a TEST_CASE name made `catch_discover` lump 15 quadrature tests into one broken ctest entry. (2) **win-tidy naming pass CLOSED** — was bigger than the old note said (it also covers the never-tidy'd TEST files): **92 violations fixed** = 53 in headers (`xgk`→`kXgk`/`data`→`kData`/local `S`/`L`/`R`/`vMax`→snake, incl. 25 late-OTG camelCase locals in `otg.hpp` never before checked) + 39 in test files (naming + isolate-decl + `1u`→`1U` + range-copy + redundant-expr). `clang-tidy --fix` proved UNSAFE on headers (corrupts template cross-refs — `abs_`, `CcX::x`; the memory scar), so headers were done manually / via controlled `\b`-boundary scripts; test files via `--fix` confined to the `.cpp` main file. **Full win-tidy check set = 0 errors across all 4 v13 test targets; win-debug still 77/77.** (3) **Conformance guard shipped** — `crd-hesap-v13-no-exceptions` (ps1+sh, registered in `tests/math/CMakeLists.txt` both arms, ctest-green, non-vacuous: 40 files, bites a real `throw`, ignores a comment `try`) = ADR-0095 pillar-3 (status-not-exception) now CI-enforced. (4) **ADR-0095 confirmed present + Accepted.** (5) **CLI `hesap.{interp,quad,diff,motion}.*` shipped** — 8 commands (interp.pchip/cubic_spline · quad.samples · diff.savgol/fornberg · motion.otg/scurve) via one `cli_register_*.cpp`+`cli_anchor.hpp` per module (added the acyclic module→crd-hesap edge where missing); **11 CLI tests green**. (6) **4 net-new system docs** `hesap-{interp,quadrature,diff,motion}.md` + the systems/README index rows written (the all-peers crush scoreboard consolidated into them). (7) **DoD configs:** **win-debug ✅ (89 v13+CLI+guard tests) · win-asan ✅ (89, ZERO ASan errors) · win-tidy ✅ (full check set 0 errors)**; the run-twice/`{1,4,16}` determinism moat is evidenced by 56 bit-identity assertions across 10+ determinism test cases. **THEN (2026-07-02, the DoD paydown):** win-shipping wiped + standalone-reconfigured (was VS-fork-poisoned); the full-engine 4-config DoD surfaced + FIXED the uncommitted v12 tree's Windows debt (`resampling.hpp` C4146 · 32 v12 test/bench tidy violations · `levin.hpp` comments tripping the transcendental guard) **and the "win-asan hang" of the fat-front NODE-PARALLEL moat test — diagnosed slow-not-hung (456s-debug test × ASan 5-6×; stacks proved compute-hot, workers parked) and root-fixed by reshaping to `bordered_spd(24,48,560)` (same guard-enforced divergent paths, 456s→9.3s debug / ~40min→<1min asan, 53214 asserts green)**. SANITY ledger 2026-07-02 + memory `feedback_timeout_is_not_a_hang_proof`. Final mole: **C4723-under-LTCG** in `otg{,_sync}.hpp` (guarded divide LTCG couldn't prove; explicit `den != 0` no-op fix, Ruckig bit-gates still green). ⭐⭐ **THE FULL 4-CONFIG DoD IS GREEN (2026-07-02): win-debug 4367/4367 · win-asan 4367/4367 (0 ASan err) · win-shipping 4280/4280 (LTCG, wiped+unpoisoned dir) · win-tidy build-clean.** **v14 TENSORS planned + sub-slice table (a–m+z, ~22.7 KLOC, all 8 user-approved additions incl. sparse/CSF+MTTKRP, cotengra-class, ReproBLAS tier, TT-cross, NN-inference/certified-tiny-ML, safetensors/DLPack, f16/bf16, randomized-decomp) written into the master phase doc; ADR-0096 + `phase-3.1.6-v14.md` at kickoff.** **v15+v16 AUTODIFF also planned (2026-07-02, all 8 frontier additions user-approved): v15 forward (a–h+z, ~10.6 KLOC — Dual/Jet + hyper-dual + SIMD vector mode + ★auto-sparsity-tracing/coloring + Giles JVPs + ★Taylor-mode jets/Taylor-ODE + ★Wirtinger) · v16 reverse (a–j+z, ~14.7 KLOC — deterministic tape + tensor/matrix VJPs + revolve + ★★implicit-diff-through-solvers [jaxopt is unmaintained ⇒ open C++ lane] + ★tape→.crds.cpp codegen + ★★deterministic-training moat + ★topology-opt showcase); ADR-0097 covers the pair at v15 kickoff.** **v17 GPU also planned (2026-07-02, maximal per user directive): a–l+z, ~20 KLOC over rhi-compute/crd-shader/ADR-0085 — coopmat GEMM · merge-SpMV · FFT-vs-vkFFT · deterministic-reduction Krylov · batched dense · Philox-on-device · NN inference (ncnn/llama.cpp-Vulkan peers) · GPU autodiff (sort-based scatter-add) · ★★the GPU DETERMINISM TIERS research slice (T1 same-device / T2 cross-arch binned-RFA / T3 cross-vendor IEEE-only + crd::math transcendentals as GLSL — publishable) · async-compute demos; NO float atomics anywhere; ADR-0098 at kickoff; depends v14 (v17-j after v16).** **FINAL round (2026-07-02, all 8 approved): quantized int8/int4+FP8 tier (INTEGER inference = bit-exact across ALL hardware, the strongest cert tier; v14-a/m + v17-i) · ★deterministic stochastic rounding (Philox SR, v14-a/c) · neural-ODE + KAN showcase (v16-k, KANs on the v13 splines) · AD-through-everything (FFT=IFFT VJP, interp/DSP; v15-f/v16-d) · ★Slang kernel track + reserved crd-rhi-cuda ABI seam (API-agnostic kernels, Vulkan first; v17-a/ADR-0098) · large single-matrix GPU SVD/eig (v17-m) · ★computation certificates (v17-k). Final planned totals: v14 ~24.1K/~880t · v15 ~11K/~455t · v16 ~16.1K/~605t · v17 ~23.1K/~690t.** **v18 NOTEBOOK + AGENT PLATFORM also planned (2026-07-02, maximal): a–i+z, ~15.7K/~490t — pulls the hesap-scoped Phase-4.0 kernel forward (crd-cli parser/REPL + crd-rpc JSON-RPC + ★the MCP SERVER, gate = a real Claude Code session driving hesap tools) + the hot-reload .crds.cpp cell engine (full-native cells vs xeus-cpp/clang-repl interpreters) + ★★the REPRODUCIBLE reactive DAG (declared typed I/O, beyond marimo/Pluto: whole-notebook run-twice bit-identity = a certification artifact w/ v17-k hash chains) + 'CNBK' pure-source workspace + in-engine plotting (sciviz boundary named) + MATLAB-class C++ ergonomics + ★write-a-cell→instant-MCP-tool (schema codegen: stubs/python-bindings/docs from the registry) + the agent-native notebook.* commands; ADR-0099 + ADR-0081 phasing amendment at kickoff.** **REMAINING:** the user commit + the 18-config CI. interp+quadrature+diff+motion are DONE + crushing. **THE 4 GAP-PACKS from the 2026-07-02 capability assessment are PINNED in `docs/ROADMAP.md`** (3.1.14 first rows = tokenizer/KV-cache/CPU-attention · NEW 3.1.18 `crd-embedded` · NEW 3.1.19 `crd-astro` · 3.1.11 += optimal-control transcription). **NEXT SESSION: (1) verify the user's commit landed + the 18-config CI verdict; (2) open v14-a — write ADR-0096 + `phase-3.1.6-v14.md` from the master-table rows, then the tensor substrate. The v14–v18 plans are FINAL (memory `project_v14_v18_planning`) — do NOT re-plan.** **POST-HESAP SEQUENCE also locked (2026-07-02, pinned in `phase-3.1-eylem.md` §Resume directive): hesap arc → EYLEM resume (the flagship consumer; crush mandate vs PhysX/Jolt full-board; two solver PROFILES on one substrate — Game f32/XPBD vs Engineering f64/implicit-over-hesap-direct, engineering = the in-house oracle) → crd-ui → editor (rides the v18 registry; gizmos cluster in the editor arc); renderer/material = consumer-pulled slices, never a standalone phase.**
>
> **THE WAY (v13 mandate — see AGENTS.md "Numerical/perf work" + SANITY #9 + Ledger 2026-06-30):** full crush of every gold-standard peer, **NO deferrals, never accept near-parity** ("parity with the same algorithm is never the wall — a per-operation cost always is"); the recurring crush lever = **precompute integrand-independent work** (nodes/weights/error-coeffs — recompute-per-call is the loss); **reconstruct-and-verify-in-python FIRST** (fetch the reference's actual source via `gh`) before porting one C++ line; **FULL peer board on every row** (install the missing peer; N/A stated with the check).
>
> **UNCOMMITTED:** all of v12 (statistics) + all of v13 (interp + quadrature + diff + motion) + this session's win-tidy renames / conformance guard is in the working tree. The user commits + runs the Windows 4-config DoD + the 18-config CI; agents NEVER commit.
>
> **═══ (the standing simulation-crush mandate + the full v5→v13 history follow) ═══**

> **g�?� STANDING MANDATE (2026-05-31): don't stop until `crd-hesap` HONESTLY + COMPLETELY crushes EVERY gold-standard solver for the simulation targets (cloth/deformation/CFD/Navier-Stokes/FEA) — Eigen + CHOLMOD + UMFPACK + PARDISO/MUMPS/SuperLU_DIST.** HONEST = fair same-class peer at its best, matched accuracy, no parallel-vs-serial asterisks. The cross-thread bit-determinism **moat** is the differentiator (beat speed AND keep determinism). Full directive: memory `feedback_full_victory_beat_all_gold_standards`.

**Committed state: `b578f66`** ("working on v9") — v7 complete + crush (`cce8e41`) and v9-a…h + v9-j(sparse-direct). **UNCOMMITTED (working tree): the v9-i/j-Krylov/k/l/z batch** (this round — see below; ready for the user's commit).

**▶ Active: v7 — the optimisation domain.** Module `crd-hesap-opt` (ADR-0090; unconstrained + constrained merged per user direction). **v7-f + v7-g + v7-h + v7-j + v7-i CLOSED 2026-06-10 (uncommitted; one session).** v7-i shipped WHOLE via a **v12-PULL** (user-chosen): new module **`crd-hesap-stats`** with the **Philox4x32-10 counter-RNG** (verified vs the published Random123 KAT vectors; pure (seed,stream,position) ⇒ moat by construction) → then ALL TEN steppers (SGD/Adam/AdamW/Nadam/RAdam/RMSprop/Adagrad/Adadelta/Lion/LAMB, PyTorch default semantics) + schedules + clipping + the **epoch-keyed reproducible MinibatchSampler** (any-visit-order replay); exact closed-form per-rule gates + two moats; 271 asserts; PyTorch-trajectory parity at v7-z. v7-f first-order (momentum/Nesterov + CG; acceleration-theorem gate; 91 asserts). v7-g Newton family (dense τ·I / Newton-CG / sparse-supernodal; τ-stuck-at-β bug gate-caught+fixed; 97 asserts, two moats). v7-h trust-region (N&W 4.1 + SIX subproblems incl. GLTR + exact-MS-via-eig; PUBLIC certified subproblem solve; 109 asserts). **v7-j constrained substrate**: `Constraints<T>` (pinned N&W conventions + curvature hook) · 4-part **KktResidual certificate** (`OptResult` grew `multipliers`+`kkt_residual`) · `solve_kkt_dense` (Bunch-Kaufman saddle + **inertia test off D's blocks** + IPOPT-style δ/γ ladders) · ℓ1 merit (+directional) · `minimize_sqp_equality` prover (with the N&W §15.6 second-order correction; HS6 deferred to v7-n as a globalization stress gate — measured ℓ1-creep, N&W prescribes damped BFGS there); 68 asserts, primal+dual moat. **v7-k ⭐ QP CLOSED**: ADMM (certified infeasibility, deterministic adaptive-ρ, polish) · Mehrotra · Goldfarb-Idnani on ONE OSQP-form with uniform duals; cross-adjudication of three independent algorithms + a permanent 400-tiny-instance GI scan (CAUGHT the GI J-transpose bug invisible on diagonal-P + the quadprog dual-carry); 1433 asserts. **v7-n ⭐ NLP COMPLETE (n-1 + n-2)**: `minimize_sqp` (damped-BFGS over the v7-k GI subproblem + ℓ1/SOC — **HS6, the deferred λ*=0 gate, <60 iters**) + `minimize_auglag` (PHR/LANCELOT over L-BFGS) + `minimize_interior_point` (**Wächter-Biegler filter IPM**: slack barrier, the reduced saddle through the v7-j inertia-corrected Bunch-Kaufman = IPOPT's own correction mechanism, filter line search, monotone Fiacco-McCormick μ, κ_Σ clip; no restoration phase — named); the battery (HS6/HS14/Rosenbrock-in-disk/circle) through ALL THREE — the circle λ* = 1−√5 now reproduced by FIVE independent algorithms; 80 asserts. **IPOPT probe DONE**: apt has Ipopt 3.11 (needs ONE sudo command from the user) → `scripts/setup-ipopt-ref.sh` ready; coinbrew 3.14+MUMPS noted for v7-z wall-clock. **v7-l LP + v7-m conic CLOSED 2026-06-10 (same session)**: `lp.hpp` — the **bounded-variable revised simplex** (two-phase artificial Phase I = infeasibility certificate, Dantzig + Bland anti-cycling, bound flips, eta-updated B⁻¹ + periodic Gauss-Jordan refactor; Beale's cycling LP terminates) + `solve_lp_mehrotra` = the v7-k IPM at P=0; EXACT dual recovery gate + 60-instance Philox simplex-vs-IPM cross-adjudication; 893 asserts. `conic.hpp` — **SCS-class ADMM** on the SCS data form (Zero/Nonneg/SOC/**PSD-via-eig_sym** cones, Π_C = b − Π_K(b − ·) on the v7-k factor seam, conic infeasibility certificates, duality-gap termination); analytic SOCP + 2×2 SDP + LP-as-conic exact-duals gates + a 25-instance scan vs the simplex (a THIRD algorithm family); 111 asserts. En route: the `#deps 0` landmine's **THIRD head** — the VS fork rewrites `CMakeCache CMAKE_COMMAND` to itself on any regenerate it executes (win-shipping re-poisoned; wiped + reconfigured, `#deps 72 VALID`); helper scripts `scripts/{build-target,configure-preset,check-deps,run-ctest}.bat` now bake the standalone-cmake policy in (CLAUDE.md). **v7-o MODELING LAYER CLOSED 2026-06-10 (same session)**: `model.hpp` `Model<T>` — declarative vars/objective/constraints as **scalar-generic lambdas** (DiffFunctor; type-erased via construct/destroy), exact forward-AD gradients + Jacobians, deterministic dispatch (SQP / L-BFGS-B / L-BFGS / auglag; bounds fold into c_I); WIRING-EXACTNESS gate = bit-identical to direct solver calls; 39 asserts. ⭐⭐ The gate **CAUGHT a real v7-n-1 SQP bug** (vertex-in-one-step ⇒ p=0 QP reported SmallStep with stale multipliers) — fixed in `nlp_sqp.hpp` (adopt QP duals + re-certify KKT before the merit machinery), zero regressions. **p/q/r DE-SLIPPED (user decision 2026-06-10): they run BEFORE v7-z; v7-p takes the FULL-PORT path** (BOBYQA/NEWUOA/COBYLA via the NLopt-C oracle + differential harness — the L-BFGS-B playbook; multi-session). **v7-p-1 CLOSED 2026-06-10**: the direct-search trio — `nelder_mead.hpp` (scipy-exact semantics + Gao-Han adaptive), `powell.hpp` (conjugate directions over a faithful Brent 1-D), `pattern_search.hpp` (GPS + OrthoMADS w/ Philox-Householder poll, mesh-size certificate); nonsmooth-ℓ1 + conjugacy + determinism gates; 62 asserts. Oracle PROBED: NLopt cloned, cobyla/newuoa/bobyqa = 1872/2583/3278 lines, per-routine diff granularity identified. **v7-p-2 COBYLA CLOSED 2026-06-10 — DIFFERENTIALLY VERIFIED (2050 checks, 0 fail)** — `cobyla.hpp` = the faithful line-for-line NLopt-reference port (f2c idiom, original gotos, exact float-literal artifacts, LCG + ENFORCE_BOUNDS + SAS-ρ + relstop verbatim; named deltas: no force/time stops, iprint stripped); functional gates incl. Powell's disk problem + **Rosenbrock-in-disk at the scipy x* (CROSS-FAMILY adjudication vs SQP/auglag/IPM)** + bit-identical-incl-LCG (23 asserts); ⭐⭐ the HARNESS (`setup-nlopt-ref.sh` oracle: exposed-statics TU + ar-d dedupe + the `crd_cobyla_e2e` rescaling-free shim; `cobyla_difftest.cpp` on WSL): **per-routine `trstlp` BIT-EXACT across 405 instances incl. the L130 duplicate-gradient paths + end-to-end bit-exact x/minf with IDENTICAL EVAL COUNTS on all 5 shared problems** (the boxed-Rosenbrock slow-crawl = the reference's own behavior, counts match). **v7-p-3 NEWUOA CLOSED — DIFF-VERIFIED (3773/0)** (classic-unconstrained scope pinned, MMA-bound variant excluded; quadratic-exactness gate). **v7-p-4 BOBYQA CLOSED 2026-06-10 — DIFF-VERIFIED (3045/0) ⇒ v7-p COMPLETE**: `bobyqa.hpp` all six routines (update/prelim/altmov/trsbox/rescue/bobyqb; bounds native; the NLopt rescaling layer excluded — equal-dx identity shim = apples-to-apples; Powell's SL/SU bound-preprocessing ported); functional gates incl. the EXACT bound landing (x[0]==1.0 bit-equal); per-routine bit-exact w/ tight-bound active-set regimes + prelim-shim + e2e identical eval counts; named gap: rescue per-routine diff (opportunistic via e2e). **v7-p TOTAL: the direct-search trio + 3 Powell ports = 8868 oracle checks, 0 failures.** **v7-q GLOBAL CLOSED 2026-06-10**: `hesap-stats/normal.hpp` (Philox Box-Muller NormalSampler; 4-moment + bit-identity gates; stats 230660/7) → `cmaes.hpp` (**Hansen-tutorial-faithful CMA-ES**: standard defaults, CSA + hσ, rank-1+rank-μ, eig_sym per gen) + `global_search.hpp` (scipy-best/1/bin DE · Clerc PSO · classical SA · basin-hopping w/ scipy AdaptiveStepsize · multi-start; dual-annealing Tsallis named not-shipped). Gates: CMA-ES Rosenbrock-5 + sphere collapse · **DE finds the Rastrigin-4 global through ~10⁴ local minima** · wrong-well escapes · CMA-ES/DE bit-identical run-twice. **v7-r MIP CLOSED 2026-06-10**: `mip.hpp` B&B over the v7-l bounded simplex (best-bound + most-fractional, deterministic ties; proven-optimum/incumbent/infeasible statuses; Gomory named not-shipped — needs tableau access); gates incl. **25 random binary instances vs EXHAUSTIVE enumeration + a permanent simplex-vs-IPM root cross-check** — ⭐⭐ which CAUGHT a real dangling-pointer bug (push_node reallocation invalidated the parent-bound pointers ⇒ a garbage child box "pruned" the true optimum; simplex exonerated first via the root cross-check; fixed + ASan green). **v7-z CLOSED 2026-06-10 (local parts) ⇒ THE v7 CLUSTER IS COMPLETE (a→z)**: CLI **`hesap.opt.{qp,lp,mip,conic}.f64`** shipped + tested (63 asserts); `docs/systems/hesap-opt.md` rewritten to the full method set; ADR-0090 stands; ⭐⭐ **the GOLD-STANDARD SCOREBOARD run on identical pinned problems** (scripts/opt_scoreboard.py vs runtime/examples/opt_scoreboard.cpp; peers pip-installed): objective agreement EVERYWHERE (QP=OSQP/quadprog to 9 decimals · LP=HiGHS · MIP=highspy proven · SOCP=scs/analytic), **EXACT trajectory matches vs scipy on NM (219=219), trust-ncg (31/28=31/28), trust-exact (27/24=27/24)**, GLTR 39 evals vs scipy trust-krylov's 401, our DE finds the Rastrigin global scipy's defaults miss, CMA-ES ≈1.2× pycma evals; + by reference: liblbfgs parity, CHOLMOD structure-dependent, the NLopt ports BIT-EXACT (8868/0). Suite was 3774/151 at z-close. ⭐⭐ **THE FULL-CRUSH PASS CLOSED EVERY SCOREBOARD GAP 2026-06-11** (session `2026-06-11-v7-full-crush.md`): (A) **Ruiz equilibration** in `solve_qp_admm` (OSQP §5, default-on, deterministic) — QP **54→31 iters** (OSQP 25, obj exact) — which EXPOSED two latent polish bugs, both root-fixed: the NaN-blind residual max-fold (garbage reported residual 0 ⇒ accepted) + missing dual-SIGN check in polish acceptance (a wrong active set solved exactly beats the certificate but flips a multiplier sign); (B) **active CMA-ES** (negative weights, formulas verified against the installed pycma source, default-on) — ros5 **1816 vs pycma 2254 WIN**, sph8 1590 vs 1391; (C) **Powell scipy-exact inner coupling** (inner Brent at 100·xtol) — **494 vs scipy 792 WIN**; (D) **BH LbfgsFd local mode** (scipy's default L-BFGS+2-point-FD + the factr flat-f exit, FD-probe-counting nfev) — **9624 vs scipy 8881 parity** (gate-caught: without the flat-f exit FD-noise burned 2.9M evals); (E) **the live torch row** — Adam AND AdamW **12-digit-identical 200-step trajectories** vs torch 2.7.1 (ours ~0 ms vs 22–780 ms). En route: win-shipping found RE-POISONED by the deps landmine (fork CMAKE_COMMAND, `#deps 0` ⇒ the first shipping green was a stale-object lie) → wiped + standalone reconfigure → honest green. Suite **3782/152**; win-debug/asan/shipping/tidy + guards green. ⭐ **THE IPOPT ROW LANDED 2026-06-11** (user supplied sudo; coinor-libipopt 3.11.9 + cyipopt 1.7.0 installed): Rosenbrock-in-disk pinned instance — IPOPT f 0.045674808/16 iters vs **Cerid filter IPM f 0.045674809/22 iters, x identical to 7 decimals** — no pending reference rows remain. ⭐⭐ **THE LATTICE KERNEL DIG (same day, session `2026-06-11-lattice-kernel-crush.md`)**: the v7-e-2 "serial kernel wall" was ⅓ wall ⅔ fixable — (1) `syrk_lower_minus` packed its operand ELEMENTWISE through MatrixView::at() (one TLB miss/element on big-ld panels) → rebuilt as a Goto-blocked triangular gemm sharing `pack_a/pack_b` + the microkernel + the EXACT gemm K-grouping (heals a latent serial-syrk-vs-parallel-gemm value divergence at knc>256); (2) the ColMajor gemm merge strided every write by ld → i-inner fix (bit-identical, universal); (3) the below-outer TRSM landed in bit-identical in-place wide-N RowMajor form (kills the Tᵀ-scratch passes). **lat32 serial 0.73×→0.85-class (4283→3705ms); lat24 8T = 0.99× PARITY; FEA improved (hood 1.57×, bcsstk25 1.85× WIN); residuals unchanged 9e-15; dense 359,508 + direct 598,861 asserts green on WSL+win-debug+shipping (+asan)**. ⚠ probe lesson: ad-hoc bench TUs MUST carry `-DCRD_SIMD_TARGET=2` or header templates instantiate the scalar microkernel (a 20× phantom). ⭐⭐ **THE SOLVE CRUSH + FULL-SCOREBOARD CORRECTION (same day, session `2026-06-11-solve-crush-full-scoreboard.md`)**: the user caught FACTOR-only victory reporting while SOLVE lost 2-3× — standing rule saved (`feedback_full_scoreboard_no_partial_victory`: ALL metrics in every verdict). Three solve root fixes: (1) single-RHS parallel solve was NET-NEGATIVE (69→98ms at 16T) → always-serial gate; (2) the backward pass was a scalar FP-add chain → shared SIMD kernels (`solve_axpy_*` bit-identical maps + `solve_dot_conj`→`simd_dot` deterministic tree) in BOTH serial+parallel paths → lat32 serial solve 70→51ms; (3) multi-RHS collapsed at 16T (lat12 x16 0.08×!) → measured work gate (lnz·nrhs ≥ 160M) + 8-lane cap → 0.83×. RESULT (intermediate): solve flat at every worker count, big lattices 0.69-0.83. ⭐⭐⭐ **THEN THE DEEP-RESEARCH DIG FLIPPED THE SOLVE TO A WIN (session `2026-06-11-multistream-solve-win.md`)**: read the CHOLMOD source (cloned SuiteSparse — its solve = dtrsv+dgemv, same structure) · pinned the bytes (trapezoids IDENTICAL 67.87M both sides — the "22% less fill" claim compared our trapezoid to their RECTANGLE count, corrected) · pinned their rate (per-pass split: 29 GB/s, ABOVE the 26 single-stream ceiling) · found the MECHANISM (measured: 1 stream 22.7 / 2 streams 29.7 / **4 streams 36.9 GB/s** — DRAM page-level parallelism; OpenBLAS dgemv = 4 columns = 4 streams) · built 4-COLUMN-FUSED solve kernels (forward fusion BIT-IDENTICAL — ascending-k per element preserved; backward = new fixed deterministic order; serial≡parallel share the helpers by construction). **SOLVE NOW WINS EVERY MATRIX AT 1T (1.06-1.47×), most at 8/16T (to 3.74×); lat32 0.50→1.14×; hood wins EVERY metric at EVERY thread count.** ⭐⭐ **THEN THE x16+FACTOR ROUND (session `2026-06-11-x16-factor-crush.md`)**: (1) the SAME multi-stream mechanism applied to `pack_a`/`pack_b` (4-way p-interleave, bit-identical copy reordering) — every cold gemm operand was a single stream ⇒ lat32 serial factor 3770→3516ms, factor 8/16T parity-to-WIN lat20/24/28 (1.02-1.04×); (2) the x16 split MEASURED (42% in the diagonal solves — quadratic dscr re-streaming) ⇒ fused mRHS kernels (`solve_mrhs_{fwd,back}_{below,diag}`: allocation/pack-free, 4-8-fused streams, r-blocked, latency-unrolled; gemm-exact fma chains) ⇒ **x16 lat24 1.28×/1.21× WIN @8/16T · lat32 1.29× WIN @16T · hood 1.67/1.52× WIN · serial x16 lat28 123→87ms**. Remaining named (in the session log with causes): lat12/20 x16 0.60-0.77 (sub-25ms problems), lat28/32 1-RHS @8T 0.83-0.90 (their threaded gemv — within-front lever), lat28/32 factor serial 0.81-0.84 (the last ~10% to OpenBLAS asm), lat12 factor @16T (worker guidance). Suites green everywhere. Also the packed-TRSM driver landed (resident-panel walk, bit-identical; B-phase ~equal — the 1240ms attributed to "B" was a MISLABELED timer wrapping the (C) trailing) + the in-place (C) syrk merge + the lat32@16T ubuf OOM root-fixed (per-worker slices vs the TLSF 4GB chunk cap) → **16T: lat24 1.02 WIN · lat28 1.00 · lat32 0.96 factor parity**. Suites green everywhere (dense 359508 + direct 598861 × WSL/debug/shipping/asan/tidy). **USER-SIDE REMAINDER: (1) the v7+kernel+solve COMMIT, (2) the 18-config CI sweep post-commit.** Plan: `docs/phases/phase-3.1.6-hesap.md`; session logs `2026-06-10-v7{f..z}-*.md` + `2026-06-11-v7-full-crush.md` + `2026-06-11-lattice-kernel-crush.md`; memory `project_v7_optimization_plan`, `project_v7e2_lattice_cholmod_perf`.

**▶ Active: v9 — ODE/DAE, new module `crd-hesap-ode` (v8 was absorbed into v7). v9-a SUBSTRATE SHIPPED 2026-06-11** (ADR-0091 Accepted; session `2026-06-11-v9a-ode-substrate.md`): the TWO API LAYERS — raw-span allocation-free stepper kernels (euler/midpoint/rk4; the eylem/animation/DAW hot-loop layer, in-place safe, bit-deterministic) + the driver substrate (`OdeFunction<T>` locked-vtable capability contract · `OdeWork` deterministic work-precision counters · WRMS norm + scipy-exact Elementary + Hairer-PI controllers · the dense-output contract + Hermite fallback · `integrate_fixed`). 109 asserts/11 cases; win-debug+guards/shipping/asan/tidy/WSL-gcc green (additive-module lean verification per user direction — CI owns the full sweep). **v9-b + v9-c + v9-g BATCH SHIPPED 2026-06-12** (session `2026-06-12-v9bcg-erk-events-symplectic.md`): `erk.hpp` RK23/RK45/DOP853 (tableaus EXTRACTED from installed scipy via `scripts/gen_erk_tableaus.py`) + Cash-Karp + Tsit5 — ⭐⭐ **scipy STEP-SEQUENCE EXACTNESS PROVEN** (`ode_scipy_difftest`: VdP 2177/6533 · 168/1106 · 55/926 == scipy EXACTLY across 3 methods, decay states BIT-identical; the ODE analog of NM 219=219) + order slopes 3/5/5/5/8 + Arenstorf closes; `solution.hpp`+`events.hpp`+`brentq` (scipy event semantics, analytic-root gates, Hermite-bound dense output); `symplectic.hpp` (symplectic Euler/velocity Verlet/Yoshida-4/6 — Kepler slopes 2/4/6, bounded-energy-vs-RK4-drift at game h=0.2, reversibility). **279 asserts/24 cases** green on debug/shipping/asan/tidy/WSL-gcc + all guards. **v9-d BDF/NDF SHIPPED same day** (same session log §v9-d): `bdf.hpp` scipy-verbatim NDF (incl. the stale-LU quirk — a wrong refactor-on-c-change draft caught against the source) + **`ode_linear_solver.hpp` THE SEAM** (dense hesap-dense LU; sparse/Krylov at v9-j) — ⭐⭐ **EVERY COUNTER IDENTICAL to scipy BDF** (ROBER 163/431/5/37 · VdP-1000 79/174/4/24 — naccept/nfev/njev/nlu all exact, y to 14-15 digits) + ⭐ **THE STIFF CRUSH: BDF 174 evals vs RK45 1,642,370 (9,439×) on VdP μ=1000**. Suite **341/30**, all configs + guards green. **v9-e Radau IIA(5) SHIPPED same day** (§v9-e): `radau.hpp` scipy-verbatim (collocation eigenbasis, real+COMPLEX hesap-dense LU per Newton iter, Gustafsson predictive + keep-LU, collocation dense output = Z0 warm start) — ⭐⭐ **EVERY COUNTER IDENTICAL to scipy Radau** (ROBER 88/726/22/110-nlu · VdP-1000 29/233/5/40; y to 16 digits / bit-identical) + L-stability + crush gates. Suite **358/34**, all configs + guards green. **v9-f RODAS4 + TR-BDF2 SHIPPED same day** (§v9-f): ⭐⭐⭐ **FOUND AND FIXED A REAL BOOST.ODEINT BUG** — odeint rosenbrock4's d4 sign (+0.0362 vs rodas.f's −0.0362) silently degrades EVERY non-autonomous problem to ORDER 1 (proven on odeint itself, five h-decades, p̂→1.0002; our verbatim port matched it digit-for-digit pre-fix, then the rodas.f sign restored p̂=4.15, error 10,000× smaller). TR-BDF2 = stiffly-accurate ESDIRK (γ=2−√2, ARKODE table, shared iteration matrix — the SPICE/DAW workhorse + the v9-i IMEX core). Bounded-cost contracts AS ASSERTIONS (RODAS4 nlu==attempts, nsol==6·attempts — no Newton). Suite **368/40**, all configs + guards green. **v9-h + v9-j(sparse-direct) SHIPPED 2026-06-13**: mass matrices/index-1 DAE through BDF (M=I path byte-identical to scipy-exact; Robertson cross-FORMULATION gate; singular-M exact) + `SparseOdeLinearSolver` over the v5b multifrontal LU — heat-2D MOL n=1024 EXACT-eigenmode gate + ⭐⭐ **THE SCALE CRUSH: n=4096 sparse-vs-sparse, Cerid BDF+multifrontal 50.4 ms/err 3.7e-9 BEATS CVODE+KLU 56.6 ms/err 1.7e-8**. Suite **405/48** (win-debug + WSL gcc green). Earlier wall board: BDF 5.9×/RODAS4 8.2× faster than CVODE (ROBER), 5.8× (VdP-1000), beats odeint dopri5 on wall+evals. **▶▶ v9-i + v9-j(Krylov) + v9-k + v9-l + v9-z BATCH SHIPPED 2026-06-13 ⇒ THE v9 CLUSTER IS COMPLETE (a→z)** (session `2026-06-13-v9-imex-krylov-sens-dae-batch.md`): **v9-i** `imex.hpp` ARK3/4/5 (Kennedy-Carpenter, coefficients EXTRACTED from SUNDIALS v6.4.1 by `gen_ark_tableaus.py` → bit-identical to ARKODE; OdeFunction slots 7/8/9 split; per-part order slopes 3/4/5 · L-stability · advection-diffusion MOL IMEX <⅓ RK45 steps). **v9-j Krylov** `ode_krylov_solver.hpp` = CVODE SPGMR (matrix-free FGMRES over jacobian_vector, NO Jacobian assembled; `OdeKrylovPreconditioner` PrecSetup/PrecSolve; new `is_matrix_free`/`factor_iteration_matrix_matfree` seam + BDF use_matfree branch; matrix-free == dense BDF to **2.8e-16**, tridiag PrecSolve 485→206 iters). **v9-k** `sensitivity.hpp` forward (CVODES augmented `[y;S]`, block-diagonal J_y stiff path) + adjoint (CVODES ASA `[λ;q]` backward) — ⭐ THREE-ORACLE gate forward=adjoint=FD. **v9-l** (NOT slipped) `dae_structural.hpp` Pryce Σ-method (≡ Pantelides; pendulum→3/index-2→2/index-1→1/ODE→0) + `dae.hpp` mechanical index-3→index-1 reduction (pendulum |constraint| 2.2e-10, energy 1.7e-9). **v9-z** CLI `hesap.ode.solve.f64` (canned × 7 methods) + system doc + ADR-0091 finalized. **Suite 577/67 on win-debug + win-asan (ZERO ASan errors on all new code) + win-shipping (LTCG).** PENDING USER/CI: the v9 batch COMMIT · the 18-config CI sweep (⚠ win-shipping deps RE-poisoned — CMAKE_COMMAND→VS-fork + English deps prefix; wipe+standalone-reconfigure before next local use) · the work-precision rows are DONE (3 new benches + run scripts). ⭐ **IMEX-vs-ARKODE crush** (`bench_ode_imex_vs_arkode.cpp`, ARKStep's own ARK4, advection-diffusion MOL): the honest **MATCHED-ACHIEVED-ACCURACY** verdict (Cerid err≈rtol; ARKODE over-solves ~3× ⇒ matched-rtol is the wrong axis) — err ≤ 1e-5 **2.38× wall / 1.55× evals**, ≤ 1e-7 **1.33×**, ≤ 1e-9 **1.14×**; Cerid WINS at every accuracy + same tightness (1.3e-10), better-calibrated. Plus stiff BDF 5.9×/RODAS4 8.2× vs CVODE + sparse-beats-CVODE-KLU (v9-d…h). ⭐ **Krylov vs CVODE-SPGMR — CRUSH** (`bench_ode_krylov_vs_cvode_spgmr.cpp`, 2D heat): the first run found Cerid doing ~5-6× more GMRES iters (fixed inner tol 1e-7) — **FIXED in-library** (inexact-Newton forcing, default 0.05) ⇒ iters 88-275→23-93, now **2.14×/1.38×/1.57× wall WIN** at matched accuracy. ⭐ **Forward sensitivities vs CVODES — CRUSH** (`bench_ode_sens_vs_cvodes.cpp`, Robertson): first run 12× SLOWER — **FIXED in-library** (exclude S from error control = CVODES sensErrCon=false + `BlockDiagonalOdeLinearSolver` factors n×n once) ⇒ 20.0ms→0.276ms, now **5.57× FASTER** (values agree 2.8e-8). NO follow-ups — every inefficiency fixed at the spot; all 5 configs (debug/asan/shipping/tidy/gcc) green 577/67. Follow-ups: native interpolants, Verner, Yoshida-8, BDF/Radau events, GGL projection, AD-symbolic auto-reduction. Follow-ups: native interpolants, Verner, Yoshida-8, BDF/Radau events, odeint d4 upstream report, calc_ic, mass×sparse. Full 13-subslice subdivision in `docs/phases/phase-3.1.6-hesap.md` (the v9 block) + memory `project_v9_ode_plan`. Spine: a substrate (dense-output + controller contracts day 1) → b explicit RK (scipy-trajectory-exact RK23/RK45/DOP853 + Tsit5) → c events → d BDF/NDF over the `OdeLinearSolver` seam → e Radau IIA → f Rosenbrock/SDIRK; g symplectic (eylem pull, independent); h mass-matrix/index-1 DAE → i IMEX → j sparse+Krylov Newton (the stack showcase: hesap-direct `refactorize` + GMRES = CVODE-KLU/SPGMR peers) → k sensitivities (CVODES pattern) → l Pantelides (SLIP candidate) → z close (CLI + the work-precision scoreboard, FULL-BOARD rule). **Oracles installed 2026-06-11: SUNDIALS 6.4.1 full suite (CVODE/CVODES/IDA/IDAS/ARKODE, apt) + Boost.odeint; scipy present; Hairer Fortran + Bari IVP-Testset fetched at their slices.**

**▶▶ v10 — FFT `crd-hesap-fft` — ✅ CLUSTER COMPLETE a→z (2026-06-20). ADR-0092; `docs/systems/hesap-fft.md`; FFT suite GREEN 259/28 (linux-gcc).** The full transform suite ships: complex FFT (`FftPlan`, Stockham + codelets + Bailey four-step) · real FFT · DCT/DST · NUFFT · **v10-c Bluestein** (any-size, chirp-z over one pow-2 plan, ~1e-15) · **v10-e N-D** (2D/3D/N-D row-column; pow-2 axis→FftPlan else Bluestein; forward-only-per-axis + N-D forward-trick inverse, ~1e-16) · **v10-h Sparse FFT** (HIKP, END-TO-END SUB-LINEAR + NOISE-ROBUST: multi-scale binary location + voting + median, no O(n) step; exact-sparse ~1e-7, noisy all-freqs-recovered to the √(n/B)σ/√R floor) · **v10-z CLI** `hesap.fft.{forward,sparse}.f64`. **Engine perf BANKED (parity DEFERRED per ADR-0092 — production-grade portable, not MKL clone):** 256K Stockham disaster FIXED + now DEFAULT (0.25→~0.85×, 3.6× CRD; the S6 four-step+16×16-hier+gather/scatter fusion wired, all rejected POCs S7/S8b/S8f stripped, engine gcc-clean) · canonical 1M/2M ~0.77-0.83× · 4M/8M ~0.92-0.95× (host-noise-caveated) · beats PocketFFT everywhere; the residual ~15% mid-size = the four-step inter-stage twiddle = the asm-integrated-butterfly gap, measured-exhausted (8 attempts), OUT OF SCOPE per ADR-0082 (portable-C++/WASM-no-asm). ✅ **FULL 4-CONFIG WINDOWS DoD GREEN 2026-06-20** (win-debug 3942/3942 + win-asan 3942/3942 + win-shipping 3855/3855 build+ctest + win-tidy build-clean; one test-helper naming violation caught + fixed). PENDING USER: commit + 18-config CI sweep. **NEXT = v11 DSP** (FIR/IIR/biquad/resampling/spectral — builds on the FFT). *(historical campaign record retained below.)*

**▶▶▶ ACTIVE: v11 — DSP `crd-hesap-dsp` (ADR-0093; `docs/systems/hesap-dsp.md`; plan `project_v11_dsp_plan` + phase-doc v11 table). 9 SUB-SLICES SHIPPED 2026-06-21 (session `2026-06-21-v11-dsp-core.md`); FULL DSP SUITE 6252/47 GREEN (linux-gcc), std-clean.** The MAXIMAL DSP subject as 3 isolated modules (`crd-hesap-dsp` core+adaptive · `crd-hesap-wavelet` · `crd-hesap-comms`) + a `crd-units` add. ⭐ Honest gate (ADR-0093): **DESIGN = spec-compliance** (transcendental ellip/remez ⇒ NOT bit-match — std::sin drifts cross-libm; gate equiripple+10-digit) · **APPLICATION = bit-exact + {1..16} MOAT** (streaming). Data-flow rule LOCKED: design in zpk→sos directly, tf output-only. **DONE: v11-a** substrate (reps tf/zpk/SOS/ss/lattice + conversions + freqz; `crd-units` DecibelRatio/Power + NormalizedFreq) · **b** 24 windows (Kaiser/DPSS/Dolph-Cheb/…) · **c** firwin/firwin2 · **d** firls + **Parks-McClellan remez** + SavGol + RRC · **e** IIR Butter/Cheb-I/II/**Bessel** + bilinear · **f ELLIPTIC (Cauer)** — ellipk/ellipj/ellipdeg/arc_jac_sc substrate each gated vs scipy.special then ellipap/ellip (elite-hard, first-try via bottom-up gating) · **i** sosfilt/sosfiltfilt + lfilter/lfilter_zi/filtfilt (bit-exact + streaming moat) · **j** convolve/fftconvolve/FftConvolver/correlate/oaconvolve/deconvolve/matched_filter · **m** welch/spectrogram/stft/istft/periodogram/csd/coherence — ⭐⭐ **THE MULTI-THREADED FFT** (parallel batched, per-job RealFftPlans + serial fixed-order reduce ⇒ bit-identical across {1,2,4,8,16} threads = a moat MKL/FFTW lack). ⭐⭐⭐ **PERF (all peers, honest):** windows 1.9-10.4× scipy / 1.2-2.7× MATLAB · firwin 8.6×/3.9× · firls 41.8×/3.4× · sosfilt 1.11× scipy/1.82× liquid · FftConvolver 2.1× scipy · **Welch 11.5× scipy / 15.3× MATLAB pwelch.** liquid-dsp 1.6.0 installed (apt). ⚠ user corrections this session (→ memory): NEVER cherry-pick perf peers; "MKL gap" was WRONG (profiled = per-call plan-rebuild, fixed via FftConvolver); NO std containers ANYWHERE incl test .inc refs (→ plain C arrays). **PENDING USER: commit v11-core + full Windows 4-config DoD + 18-config CI.**
**▶▶ v11-g/h/k/l/r/t/n/o/p/q BATCH SHIPPED 2026-06-21 (session `2026-06-21-v11-dsp-g-h-k-l.md`, UNCOMMITTED). Full DSP suite 12305/81 GREEN (linux-gcc).** **v11-g** IIR digital design (`iir_design.hpp`: lp2hp/bp/bs freq-transforms + `iirfilter`/`iirdesign` + order-est buttord/cheb1ord/cheb2ord/ellipord incl. a faithful `fminbound` Brent port for bandpass + iirnotch/iirpeak/iircomb — exact N + Wn + spec-compliance + response-equality vs scipy; ⭐ caught a `band_type_of` bandpass/bandstop inversion via the spec gate) · **v11-h** RBJ audio biquads (`rbj.hpp`: LPF/HPF/BPF/notch/APF/peaking/low+high-shelf + cascade EQ; coeffs vs independent cookbook 1e-12 + DC/Nyq/centre spec properties; raw f64 kernel layer) · **v11-l** Hilbert (`hilbert.hpp`: analytic signal pow-2-FftPlan/any-size-Bluestein + envelope + inst-phase/freq + plan-cached `HilbertTransformer`) · **v11-k** multirate (`multirate.hpp`: `upfirdn` reversed-polyphase-bank + `resample_poly` + `decimate`). ⭐⭐ **PERF (all gold standards, 1-thread, chk-identical ⇒ correct):** resample_poly 1M up3/2 **Cerid 10.76ms vs scipy 1.17× / MATLAB 2.18× / liquid-f32 1.15×** · up2/3 **1.08× / 1.99× / parity** (EARNED via reversed bank; first cut LOST 0.84/0.69×) · Hilbert cached **N=1M 17.46ms vs scipy 1.45× / MATLAB 1.81×** (one-shot loses on plan-rebuild — the FftConvolver lesson; cached = the fix). g/h = design slices (no perf bench per the honest-gate). · **v11-r** waveforms (`waveforms.hpp`: chirp[lin/quad/log/hyperbolic] + sawtooth/square + gausspulse + sweep_poly + unit_impulse; generators ⇒ vs scipy ~1e-10, no perf bench) · **v11-t** adaptive (`adaptive.hpp`: `LmsFilter`/`NlmsFilter`/`SignLmsFilter`/`RlsFilter` alloc-free stateful + run-twice moat + `wiener_hopf`; gate = known-plant recovery; ⭐⭐ **PERF: LMS 22.6ms beats liquid eqlms 1.87× [f64 vs f32] / MATLAB dsp.LMSFilter 1.18×**). ⭐ Phase doc consolidated to ONE v11 sub-slice table (removed the duplicate deep-dive tables per user request). · **v11-o** AR (`ar.hpp`: aryule/arburg/ar_psd/AIC-MDL; known-AR(2) recovery; ⭐ caught a Burg backward-error index bug; **PERF arburg 4.55× MATLAB**) · **v11-q** transforms (`transforms.hpp`: goertzel/czt/rceps/cceps/fwht; ⭐ caught a Goertzel output-formula bug; **PERF czt 3.59× MATLAB, 0.49× scipy — M=N shortcut**) · **v11-p** subspace (`subspace.hpp`: music_spectrum/root_music; ⭐⭐ resolves Δf=0.006 tones BELOW the FFT bin = super-resolution) · **v11-n** multitaper (`multitaper.hpp`: K-DPSS averaged eigenspectra; tone peak + variance reduction; accuracy-gated, taper-setup eig-bound). **NEXT IN ORDER = v11-s** (detection + measurements: find_peaks + SNR/THD/SINAD/SFDR/ENOB) → wavelet → comms → z-close.
**▶▶ FOLLOW-ON SWEEP (2026-06-21, user: "NO FOLLOW-ONS, implement everything", UNCOMMITTED, suite 12576/87):** ⭐⭐ **CZT CRUSH** — added plan-cached `CztPlan` (caches FFT plan + chirp-FFT): **cached czt 0.0859ms BEATS scipy cached 0.0968 (1.13×) + MATLAB ~25×** (was losing 0.49× one-shot). ⭐ **ROOT-CAUSE FIX in crd-hesap-dense**: `eig_real_impl`/`eig_complex_impl` lacked an **n==1 guard** (1×1 matrix) ⇒ SIGSEGV in `hessenberg` — exposed by `residuez` calling `roots()` on a degree-1 poly; fixed at the root (benefits every eig consumer). ⭐⭐ **ALL FOLLOW-ONS CLEARED (user: "NO FOLLOW-ONS"); suite 13274/94 GREEN; NO malloc (confirmed — crd containers only).** Round 1: **v11-t** `ApFilter` (affine projection) · **v11-o** `arcov`/`armcov` (covariance + modified-covariance AR) · **v11-q** `impz`/`residuez`/`lifter` (inverse-Z + partial fractions + liftering) + cached `CztPlan` · **v11-r** `mls`/`gold`/`kasami` (PN sequences). Round 2: **v11-p** `esprit`/`min_norm` (super-resolution, unblocked by the eig n==1 fix) · **v11-n** adaptive eigenvalue weighting (concentration eigenvalues + Thomson iteration) · **v11-k** `interp`/`resample`(FFT)/`half_band`/`cic_decimate`/`farrow_delay` · **v11-l** `hilbert2` (2D analytic — ⭐ root-caused the scipy single-orthant Nyquist-zeroing convention) · **v11-d** `remez_hilbert`/`remez_differentiator` (type-III antisymmetric Parks-McClellan). **NO follow-ons remain in v11-dsp-core.** ⭐⭐ **v11-s SHIPPED (suite 13785/98): `measurements.hpp` — find_peaks/peak_prominences/peak_widths/argrelextrema (exact vs scipy) + detrend + rms/crest + thd/snr/sinad/sfdr/enob + cspline1d; PERF detrend 13.3× scipy / thd-snr ~6× MATLAB / find_peaks parity. ⭐ THE ENTIRE v11-dsp-core (a–t) IS COMPLETE.** NEXT = the **`crd-hesap-wavelet`** module (v11w-a families/filterbanks → b DWT/IDWT → c SWT/WPT → d CWT → e 2D/denoising/MODWT, vs PyWavelets) → **`crd-hesap-comms`** (v11c-a…g) → v11-z close (CLI + system docs + scoreboard). **PENDING USER: commit + full Windows 4-config DoD + 18-config CI.**
**▶▶ `crd-hesap-wavelet` MODULE COMPLETE — NO FOLLOW-ONS 2026-06-21 (v11w-a…e ALL SHIPPED; session `2026-06-21-v11w-wavelet-module.md`). NEW module; UNCOMMITTED. Wavelet suite 14943 asserts/23 cases GREEN (linux-gcc) + CI guards green.** Built vs PyWavelets 1.8.0: **a** families (76 wavelets, coeffs GENERATED verbatim from pywt → machine-precision) + QMF · **b** DWT/IDWT/wavedec/waverec + ALL 9 boundary modes (per-mode coeffs BIT-MATCH pywt 1e-11; probe-pinned Haar→db2) · **c** SWT à trous + iSWT + WaveletPacket + best-basis · **d** CWT **FULL family** (mexh/morl/cmor/gaus1-8/cgau1-8/shan/fbsp/paul, ψ=pywt; paul analytic) + generic central_frequency (FFT-peak) + ridge, **MT-batched over scales** · **e** 2D dwt2/idwt2/wavedec2/waverec2 (**MT rows+cols**) + denoising (soft/hard/garrote + VisuShrink/BayesShrink/**SureShrink**) + **MODWT/imodwt** (Percival-Walden). ⭐⭐ PERF (chk-IDENTICAL vs pywt ⇒ correct): **wavedec db4 1.14-1.21× · swt 1.18× · CWT morl 1.17× · CWT cmor 2.94× (MT-batched complex crush) · dwt2 1024² 1.49×** + {1,4,16}-thread bit-identical determinism moat (pywt/MATLAB lack). ⚠ crd::containers::StringView + nth_element (cerid equivalents). NEXT = `crd-hesap-comms` (v11c) → v11-z close. PENDING USER: commit + Windows 4-config DoD + 18-config CI.
**▶▶ `crd-hesap-comms` MODULE COMPLETE 2026-06-21 (v11c-a…g ALL SHIPPED; session `2026-06-21-v11c-comms-start.md`). NEW module; UNCOMMITTED. Comms suite 39592 asserts/26 cases GREEN (linux-gcc) + guards. CRUSHES liquid-dsp on every benchmark.** Edges → hesap-dsp/fft/stats. Gold standard = liquid-dsp 1.6.0 + theoretical AWGN BER. **a** modulation (Gray PSK/QAM/PAM + FSK + soft LLR + O(1) slicing demod; gate=Gray+round-trip+unit-energy+**BER-vs-theory**) · **b** pulse shaping (RRC/RC/Gaussian + matched filter; gate=**RRC Nyquist zero-ISI**) · **c** timing (Gardner/M&M + SymbolSync interpolating PI loop; gate=S-curve+locks fractional delay) · **d** carrier (CostasLoop + PLL + M-th-power AFC; gate=removes phase+freq offset) · **e** equalizers (LMS-DD + **CMA blind** + DFE + **MLSE-Viterbi**; gate=open multipath BER→0, MLSE==brute-force) · **f** channels (AWGN/Rayleigh/Rician deterministic-Philox) + AGC + framing + Hamming(7,4) FEC · **g** OFDM (IFFT+CP/FFT on v10 + pilot-est + ZF-EQ; gate=multipath-thru-CP BER 0). ⭐⭐ PERF (Cerid f64 vs liquid f32, ALL WINS): modulate **4.2×** · demodulate **3.6×** · eqlms **2.5×** · rrc-interp **1.6×** · OFDM **3.0×**. ⭐ honest-gate: modem correctness = BER-vs-theory + Nyquist (NOT liquid's constellation rotation) ⇒ liquid = perf peer.
**▶▶▶ v11-z CLOSED + VERIFIED 2026-06-21 ⇒ THE ENTIRE v11 DSP CLUSTER IS COMPLETE (dsp a–t + wavelet a–e + comms a–g).** CLI registered for all 3 modules (`hesap.dsp.{welch,resample}`, `hesap.wavelet.{dwt,denoise}`, `hesap.comms.qam.{modulate,demodulate}` — anchor + `CRD_HESAP_CLI_REGISTER_MODULE` pattern + per-module `test_cli.cpp`). System docs `hesap-{dsp,wavelet,comms}.md` + systems README rows + ADR-0093 (Accepted). ⭐ **VERIFICATION (gcc + clang-cl + win-release + STRICT win-tidy, per user — not all 18, CI does the rest): ALL 4 GREEN** — dsp 27069/100 · wavelet 14951/24 · comms 39663/27 on linux-gcc + win-clang-cl + win-release; all 4 sanity guards (no-non-ascii/no-std-sort/no-untagged-physical/no-malloc) PASS; **the local-strict `win-tidy` (warnings-as-errors ON) builds CLEAN** after a full readability cleanup of the v11 test suite (the committed dsp tests had never run win-tidy — every v11 session built linux-gcc binaries only). ⚠ found+fixed this pass: a clang-cl unused-var · the non-ascii-test-name guard (em-dash/⇒/≈/±/₀ in 8 dsp test names → ASCII) · one comms test delay/decision name collision · the readability cleanup (isolate-declaration multi-decls split, capitalized math locals lowercased — freq-response/CZT outputs renamed hf/xf to dodge h/x collisions, literal-suffix-case, unused using/alias removed, token-pasting test macros NOLINT'd). ⚠⚠ LESSON: clang-tidy `--fix` with `readability-identifier-naming` CORRUPTS code via naming collisions (H→h/X→x/D→d collide with existing lowercase vars) — safe auto-fixes = isolate-declaration + uppercase-suffix only; naming must be manual. PENDING USER: commit + 18-config CI.
**▶▶▶ v12 — STATISTICS ✅ COMPLETE a→z (2026-06-28; ADR-0094).** ⭐ **NEXT ACTIVE: v13 — the Numerical-Analysis + Motion cluster** (interpolation · adaptive quadrature · numerical differentiation · trajectory generation) — RE-SCOPED to a MAJOR 4-module certification-grade cluster (`crd-hesap-interp` + extend `crd-hesap-quadrature` + `crd-hesap-diff` + `crd-hesap-motion`) for satellites/drones/robots/self-driving/games; 3 moat pillars = determinism-by-construction + allocation-free bounded-recursion streaming + error-tier-exposing (the DO-178C/ISO-26262-ASIL-D moat GSL/Boost structurally lose). **DETAILED PLAN `docs/phases/phase-3.1.6-v13.md`; ADR-0095 WRITTEN+indexed; main slice table expanded to every sub-slice a→z.** ▶ **v13-a ✅ DONE 2026-06-29** (`crd-hesap-interp` stood up: linear/nearest/cubic-Hermite/**PCHIP** + the `Interpolator`/`InterpStatus`/caller-workspace contract + Tier-2 linear bound; 53 asrt GREEN ≤1e-12 vs scipy + the no-overshoot invariant + determinism + status-validation). ▶ **v13-b ✅ DONE 2026-06-29** (`cubic_spline.hpp`: natural/clamped/not-a-knot/periodic via an O(n) Thomas solve, scipy-exact; 78 asrt ≤1e-12 vs scipy.CubicSpline + analytic C² + Hall-Meyer 5/384·h⁴ Tier-2 bound + determinism; full interp suite 131). ▶ **v13-c ✅ DONE 2026-06-29** (`akima.hpp`+`barycentric.hpp`: Akima/makima + barycentric-Lagrange + Newton-DD + Floater-Hormann; 121 asrt ≤1e-12/1e-10 vs scipy + Chebyshev-Runge no-blow-up + determinism; full interp suite 252). ⭐⭐ **BENCH: makima build 11.5×scipy/4.7×MATLAB · barycentric build 110× · Floater-Hormann build 1421× · evals 1.8-9.1× vs scipy.** ▶ **v13-d ✅ DONE 2026-06-29** (`spectral.hpp`+`rational.hpp`; interp now links fft+dense+hesap): Chebyshev (DCT-II + Clenshaw + vectorized eval_batch) + trig/Fourier (rfft) + rational/Padé (dense::lstsq, scipy-exact + spurious-pole guard); 24 asrt ≤1e-12/1e-10 vs numpy/scipy + band-limited exactness + determinism; full interp suite 276. ⭐⭐ **BENCH: Chebyshev build 24.8×numpy · Padé build 54.2×scipy/eval 2.8× · Chebyshev batch-eval 3.4× (vectorized fix turned a numpy loss into a win, bit-identical).** ▶ **v13-e ✅ DONE 2026-06-29** (`rbf.hpp`): scattered N-D RBF (6 kernels Gaussian/IMQ/Multiquadric/thin-plate/cubic/**Wendland-compact** + Shepard/IDW; augmented saddle-point over dense **LDLᵀ**); 81 asrt ≤1e-8 vs scipy.RBFInterpolator + interpolation property + thin-plate poly-reproduction + determinism; full interp suite 357. ⭐⭐ **BENCH: RBF build 1.29× / eval 1.12× vs scipy — both started as LOSSES (lstsq-QR + redundant sqrt), SOLVED via LDLᵀ saddle-point solver + r²-kernel sqrt-elimination (no deferrals).** ▶ **v13-f ✅ COMPLETE 2026-06-29** (`grid.hpp`+`kriging.hpp`+`clough_tocher.hpp`): N-linear + bicubic + cubic-B-spline ✅ (`RegularGridInterpolant` — `eval_linear`/`eval_cubic`/`eval_bspline` over a shared `eval_4tap` core + Unser prefilter; 115 asrt vs scipy.RGI/MATLAB interpn/scipy.ndimage + Boost; interp-property + clamp-no-NaN; full interp suite 472). ⭐⭐ **BENCH all peers: linear 3.65ns = 20×scipy/3.1×MATLAB/edges-Boost · bicubic 9.9ns = 13×scipy/1.14×Boost-1D · B-spline 8.45ns = 5.1×scipy.ndimage/1.46×MATLAB-spline/1.23×Boost (FULL crush).** ⚠ peer-completeness HARD RULE: bench scipy+MATLAB+Boost on EVERY row, state N/A with the check. **kriging/GP ✅** (`GaussianProcessInterpolant` mean+predictive-variance over `dense::Cholesky`; 17 asrt ≤1e-8/1e-7 vs sklearn; FIT 10.5×sklearn/237×MATLAB-fitrgp · predict 2.45×sklearn/1.06×MATLAB; forward-only variance solve 1−‖L⁻¹k*‖²; Boost N/A). **Clough-Tocher C¹ ✅** (`CloughTocher2DInterpolant` over `geometry-delaunay` — BIT-EXACT transcription of scipy `_interpnd.pyx`: affine-invariant neighbor-centroid reduced direction + 19 precomputed Bézier ords/tri + curvature-min Gauss-Seidel gradient; 55 asrt ≤1e-9 vs scipy + interp-property + EXACT linear-repro + C¹-smooth + outside-hull-NaN + determinism + dense-40k-interior no-NaN/order-independence; ⭐⭐ FIT 107µs = 2.14×scipy · EVAL 85ns/pt = 1.70×scipy-batch/35.7×scipy-per-point (fast non-adaptive orient2d walk + exact linear-scan fallback, C⁰-safe ⇒ no interior NaN) · 2.90×MATLAB griddata('cubic') · Boost N/A). **▶▶ v13-f COMPLETE — all 5 gridded/scattered methods crush every available peer; full interp suite 544.** **▶ v13-g ✅ DONE 2026-06-30** (extend `crd-hesap-quadrature`): **gauss_lobatto / gauss_radau** (Jacobi-root nodes + closed-form weights, exactness-verified ≤1e-13 vs scipy) + the **integrate()/QuadResult error-tier contract** + integrate_gauss/lobatto/radau + **integrate_with_nodes / integrate_symmetric** (precomputed real-time paths) + **newton_cotes** (exact Lagrange-integral) + **trapezoid** (uniform+non-uniform) + **simpson** (scipy-exact BOTH parities). 170 asrt + exactness-degree + positive-weight + determinism; full quad suite 300. ⭐⭐ **CRUSH per-call: gauss-sym 66ns = 85× scipy fixed_quad / 1.02× Boost gauss<10> (symmetric-pair fix flipped Boost's initial 4% edge — never document a loss) · simpson 21.9×scipy · trapezoid 70×scipy / 32.7×MATLAB trapz · newton_cotes 2.97×scipy** (Boost/MATLAB have no composite/Lobatto/Radau/NC peer). **▶ v13-h CORE ✅ DONE 2026-06-30** (`gauss_kronrod.hpp` + `adaptive.hpp`): **GK21 rule** (21-pt Kronrod / 10-pt Gauss embedded — integral + error estimate at no extra evals; QUADPACK dqk21 constants, scipy-bit-exact value + error incl. the roundoff floor; degree-31 exactness) + **adaptive QAG** `integrate_adaptive` (globally-adaptive bisection via an ITERATIVE bounded-depth WORK-STACK — NOT recursion; hard WCET bound = max_subiv → MaxSubdivisions, the MISRA/cert differentiator GSL's recursive qag loses). 71 asrt: GK exactness + adaptive ≤1e-10 vs scipy.quad + the no-recursion/WCET guard + the **Lyness-Kaganove honesty test** (a peak narrower than the node spacing fools the estimate ⇒ proves error_estimate is Tier-1 ESTIMATE not a bound) + determinism; full quad suite 371. ⭐⭐ **adaptive QAG 256 ns/call = 22.7× scipy.integrate.quad(5805).** **▶ QAGS ✅ DONE** (`qags.hpp`): the **Wynn-ε extrapolation** adaptive integrator (scipy.quad's default) — a faithful goto-preserving port of QUADPACK dqagse + dqelg (ε-algorithm) + dqpsrt from scipy's `__quadpack.c`; converges the endpoint-singular integrands plain QAG stalls on (∫1/√x=2 / ∫ln x=−1 / ∫(1−x)^−½=2 / ∫x^−0.8=5 ≤1e-8). 81 asrt `[v13-h]`, full quad suite 381. ⭐⭐ **QAGS 1/√x 919 ns = 37.2× scipy.integrate.quad.** **▶ QAGI ✅ DONE** (`integrate_qagi` + `gk15i` — infinite ranges via the dqagie/dqk15i transform + the shared Wynn-ε driver + a reusable `AdaptiveWorkspace`; ∫₀^�?e^−x²=√π/2, ∫_{−�?}^�?1/(1+x²)=π, etc. ≤1e-8). 95 asrt `[v13-h]`, full quad suite 395. ⭐⭐ **FULL PEER BOARD — CRUSHES EVERY peer incl. GSL (scipy+MATLAB+Boost+GSL 2.7.1): QAGS 631ns = 54×scipy / 1.53×Boost-GK-adaptive / 1.29×GSL-qags · QAGI 1076ns = 23×scipy / 1.36×GSL-qagiu** (+ the determinism/WCET/error-tier MOAT GSL lacks). ⚠ the initial GSL "near-parity" was a Cerid inefficiency not the algorithm: `crd::math::pow(·,1.5)` (heavy double-double, ~290ns/call) → `x·√x` (one hardware sqrt) flipped 0.88×→1.29× / 0.89×→1.36×, value unaffected. **▶ QNG + QAGP ✅ DONE 2026-06-30 ⇒ v13-h METHOD SET COMPLETE (QNG·QAG·QAGS·QAGP·QAGI + GK21/GK15).** QNG (non-adaptive Patterson 10/21/43/87, savfun reuse, ≤87 evals) + QAGP (break-points → split-at-singularities → QAGS pieces). 110 asrt `[v13-h]`, full quad suite 410. ⭐⭐⭐ **EVERY METHOD CRUSHES GSL: QNG 133ns=1.09× · QAGS 631ns=1.29× · QAGP 1200ns=1.24× · QAGI 1076ns=1.36× · QAG 256ns=22.7×scipy** (+ scipy 22-54×, Boost-GK 1.53×, + the determinism/WCET/error-tier MOAT). ⏳ optional (not a row requirement): higher GK31/41/51/61 (alternative QAG panels, low value — GK21 is the universal default). **▶ v13-i DE ✅ DONE 2026-06-30** (`de.hpp` — tanh-sinh/exp-sinh/sinh-sinh double-exponential; precomputed `DeRule` nodes + level-refinement). 13 asrt, full quad suite 423. ⭐⭐⭐ **CRUSHES BOOST ALL 3: exp_sinh 351ns=1.17× (THE target) / sinh_sinh 482ns=1.21× / tanh_sinh 102ns=1.34×** (levers: double-exp convergence estimate d²/dₘ₋₁ halves levels + per-run tail truncation; FIRST measured 0.48× = 2× slower, MEASURED-not-guessed fix). **⇒ the WHOLE quadrature board (v13-g/h/i) has ZERO open comparisons — every method crushes every available peer (scipy/MATLAB/Boost/GSL).** **▶ v13-i CC/Fejér/Romberg ✅ DONE 2026-06-30** (`nongauss.hpp`): Clenshaw-Curtis (clencurt weights, fixed-rule + nested-adaptive) + Fejér-1 + Romberg (function + `romberg_samples`=scipy.romb). +42 asrt, full quad suite 465. ⭐⭐⭐ **CRUSH: Romberg 207ns=1.19× GSL-romberg / ≫scipy.romb · CC-adaptive 272ns=2.05× GSL-cquad** (precomputing the per-call-recomputed clencurt weights flipped a 0.59× loss → 2.05×). ⇒ **v13-i FULLY COMPLETE; the ENTIRE quadrature module (v13-g/h/i) has ZERO open comparisons — every method crushes every available peer (scipy/MATLAB/Boost/GSL).** NEXT v13 = `crd-hesap-diff` (Fornberg/complex-step/Savitzky-Golay) → `crd-hesap-motion` (SQUAD/clothoid/min-snap/Ruckig). ~13 KLOC / multi-session. (v12 detail ↓.)** Scope: FULL elite — NEW leaf `crd-hesap-special` (gamma/beta/erf + incomplete + inverses = the cdf/ppf engine · Bessel/Airy · orthogonal polys + Golub-Welsch · hypergeometric/Lambert-W/zeta/Ei-Si/Fresnel/Struve/Marcum-Q/Carlson) + EXPAND `crd-hesap-stats` (counter-RNG · ~50 distributions · descriptive/bootstrap/KDE/MCMC/regression). ~19 sub-slices (a→z) · multi-session — DON'T marathon. Honest all-peers bench: MATLAB R2026a + scipy + Boost + GSL + NumPy. Pins D(stat)-1..5. **▶ v12-a SHIPPED** (`crd-hesap-special`): gamma/lgamma/digamma/trigamma/polygamma · beta · incomplete gamma/beta + inverses · erf/erfc/erfcx/erfinv/erfcinv · Dawson/Faddeeva/Voigt. Suite 400414/16 GREEN vs scipy refs + std cross-check + {1,4,16} moat + f32. **CRUSHES Boost on all 7**; beats scipy 5/7 + MATLAB-1T 5/7. **▶ Reusable SIMD primitives** (`detail/simd_{log,exp,lanczos}.hpp`, all bit-identical scalar↔SIMD via `-ffp-contract=off` ⇒ moat holds): SIMD log 3.11× · SIMD exp deg-11 Taylor <1e-13 · Lanczos lgamma 2.07× (vectorizes; the Stirling-recurrence form was 0.65×, rejected); tgamma=exp(lgamma). **▶▶▶ WORKER-AFFINITY ROUTING SHIPPED (gated dual-path, ADR-0094):** opt-in `jobs::Config::pcore_routing` (default false ⇒ historical shared-semaphore wake path runs VERBATIM — every shipped system/bench unaffected; jobs suite 29236/88 proves it). ON ⇒ per-worker targeted-wake scheduler (per-worker `counting_semaphore` + idle-mask + 1 ms timeout backstop = deadlock-proof) + worker→P-core affinity at init (Win EfficiencyClass GroupMask / Linux cpufreq cpu-ids; no-op on WSL) + `parallel_for_pcores`; batch.hpp routes bandwidth-bound (erf/erfc/erfcx/digamma/lgamma/tgamma)→P-cores, compute-heavy (erfinv/erfcinv/gammainc/betainc)→all cores. New path tested byte-identical to scalar refs + no deadlock. ⭐ **TRANSPARENT single-run (ONE pool) vs MATLAB-MT: lgamma 1.27× · tgamma 1.44× · erfinv 1.45× · erf 1.48× · digamma 2.30× · gammainc 14× WIN** (erfc ~parity on WSL = no-affinity noise; wins 1.12× on the clean P-core pool + reliable on Windows-affinity) ⇒ **6–7/7 transparent in ONE pool; lgamma/tgamma solidly won.** **▶ v12-b SHIPPED 2026-06-22** (`bessel.hpp`+`airy.hpp`): cyl J/Y/I/K + derivatives + negative orders (Steed/Temme CF method, the GSL/NR gold standard — continued fractions + Γ, no fragile coefficient tables) · spherical j_n/y_n · Hankel H^(1,2) · Airy Ai/Bi+deriv (Bessel-1/3,2/3 connection) · Kelvin ber/bei/ker/kei (series + complex-K₀ asymptotic large-x). **+ complex-argument Bessel** (J/Y/I/K + Hankel for complex z, all orders: series moderate-|z| + Hankel/I-K asymptotic large-|z| + EXACT integer-order Y_n/K_n DLMF series — no ε-hacks). **Full special suite 401377/25 GREEN** gated vs scipy <1e-12 real / <1e-9 complex (+ Wronskian self-checks + f32). ⭐⭐ **ALL-PEERS BESSEL CRUSH (single-thread, ns/call): Cerid WINS ALL 6 vs ALL 3 peers = 18/18** — vs Boost (cyl_J 2.2× · Y 1.03× · I 1.58× · K 1.79× · airy 8.5×/9.3×), vs scipy (J 4.1× · Y 2.4× · I 1.45× · K 1.58× · airy 1.72×/1.24×), vs MATLAB-1T (J 5.3× · Y 3.4× · I 1.5× · K 1.7× · airy 5.7×/5.4×). Levers: dedicated J-only fast path (series+Hankel-asymptotic, no quartet) + direct Airy Maclaurin series (no Bessel-⅓ route) + Steed/Temme CF for Y/I/K. ⭐ **PARALLEL BATCH (`cyl_bessel_*_batch`/`airy_*_batch`) vs MATLAB-MT (all cores): WINS ALL 6** (cyl_J 2.61× · Y 2.66× · I 1.63× · K 1.24× · airy 5.02×,2.99×) + {1,4,16} moat ⇒ Cerid Bessel crushes EVERY peer at EVERY threading = **24/24** (18/18 single-thread + 6/6 MT). ⚠ Kelvin ker/kei crossover (x~6–12) dbl-prec-limited ~1e-8 (documented). **▶ v12-c (PART 1) SHIPPED 2026-06-22** (`orthopoly.hpp`): classical orthogonal polynomial EVALUATION via stable 3-term recurrences — Legendre + associated (lpmv/Condon-Shortley) · Hermite physicist Hₙ + probabilist Heₙ · Laguerre + generalized Lₙ^α · Chebyshev 1st–4th (T/U/V/W) · Gegenbauer Cₙ^α · Jacobi Pₙ^{α,β}. Suite 401837/30 GREEN, gated vs scipy <1e-12 + analytic identities + f32. **▶ v12-c (PART 2 = Golub-Welsch) SHIPPED 2026-06-23** in a NEW module **`crd-hesap-quadrature`** (`quadrature/gauss.hpp`) — Gauss-Legendre/Hermite/Laguerre(+gen)/Jacobi/Gegenbauer/Chebyshev nodes+weights, **REUSING `crd::hesap::dense::eig_sym`** (no bespoke QL — SANITY rule 8) + hesap-special recurrences/Γ/B. Non-leaf module (needs the eigensolver) ⇒ hesap-special stays a leaf. Gated vs scipy `roots_*` <1e-11 + degree-(2n−1) exactness self-check (130/2 GREEN). ⭐⭐ **ARCH RULE ADDED (user, 2026-06-23): SEARCH THE ENGINE BEFORE YOU BUILD — reuse>reimplement** (SANITY.md rule 8 + CLAUDE.md hard-rule-1; v12 had reimplemented erf/erfc/lgamma + misplaced f64 SIMD log/exp — now MOVED to `crd/math/simd/transcendental.hpp`, its home). **▶ v12-d COMPLETE 2026-06-23** (transcendental tail; full special suite **402045/41 GREEN**, all gated vs scipy <1e-12 + identities + f32): `expint.hpp` E₁/Eₙ/Ei/Si/Ci · `elliptic.hpp` Carlson R_F/R_D/R_C/R_J + K/E/F/E/Π (CANONICAL home; hesap-dsp's `ellipk`/`ellipj` are a filter-design subset — consolidation noted) · `fresnel.hpp` S/C · `lambertw.hpp` W₀/W₋₁ · `zeta.hpp` Hurwitz/Riemann (Euler-Maclaurin) · `struve.hpp` H/L (series + Y_ν asymptotic) · `hypergeom.hpp` ₀F₁/₁F₁(Kummer)/₂F₁(Pfaff) · `marcum.hpp` Q_M (Poisson-weighted, REUSES gammainc_q). **▶ v12-d FOLLOW-ONS ALL IMPLEMENTED 2026-06-23** (user: "no follow-ons"): ₂F₁ **degenerate log forms** (DLMF 15.8.8, exact for integer c−a−b/b−a) + 1−z connection + Pfaff-recursion for |z|>1 · **zeta s<1** (Euler-Maclaurin IS the analytic continuation: ζ(0)=−½, ζ(−1)=−1/12, ζ(½) all gold) · **general pFq** · **Jacobi sn/cn/dn** (`ellipj`, NR sncndn — canonical home) · **AGM K/E** (Carlson→AGM, ellint_E 158→39 ns) · SIMD-log test moved to tests/math (its home). ⭐⭐ **v12-d transcendental-tail CRUSH vs Boost — ALL 7 now WIN (2026-06-25), were 0.05×–0.91× losses** (user: "we can crush them all"): **E1 7.98× · Ei 6.55× · zeta 20.5× · lambertW0 1.17× · ellint_K 1.06× · ellint_E 1.06× · Carlson_RF 1.78×**, accuracy preserved (special 402081 + DSP 27069 + stats 317795 GREEN, gcc+MSVC). HOW = generated minimax rationals (Python Chebyshev-fit + Lawson reweight → monomial `.inc` + shared `poly_eval.hpp::horner_t`, each gated to the fn's own tolerance): E1/Ei = −ln x+A(entire) then e^∓x/x·rational pieces (the x=0 log-branch ⇒ rational not poly) · zeta = two rationals replacing 8 `std::pow` · K/E = full-range Cody A(m1)+(−ln m1)B(m1) · lambertW0 = 3-piece rational (no Halley) · Carlson = `errtol` loosened to the 1e-12 gate (one fewer duplication). Gen scripts `tests/hesap-special/gen_{expint,zeta,elliptic,lambertw}_poly.py`; coeff `.inc`s under `engine/.../special/`. ⚠ meta-scar (SANITY ledger): my eval-cost pessimism predicted "wall/parity" 7/7 times and lost every time when MEASURED. (Earlier loss text retired — was a real loss, now solved per SANITY rule #9.) Hot fns (gamma/erf/Bessel 24/24) already crushed. ⚠ genuine f64 limit (NOT a fixable follow-on): Struve-H crossover x~14–30 ~1e-10 (asymptotic optimal-truncation). ✓ **hesap-dsp consolidation DONE**: its filter `ellipk`/`ellipj` now delegate to hesap-special's canonical ones (duplicate eliminated); v11 DSP elliptic-filter suite re-verified GREEN (107 asrt). Full special suite 402081/37 GREEN. **ALL v12-d follow-ons implemented (user: no follow-ons).** **▶ v12-e SHIPPED 2026-06-24** (RNG suite, `crd-hesap-stats`): BitGenerator concept + SplitMix64 · Xoshiro256**/++ (+jump/long_jump) · SFC64 · PCG64-DXSM · Threefry4x64-20 (seekable) · MT19937 (+ existing Philox) + Lemire bounded. **Bit-exact-gated** (31052 asrt): published KATs (SplitMix/Threefry-zero-KAT/MT19937-canonical) + NumPy random_raw set-state (PCG64-DXSM/SFC64) + xoshiro anchor; determinism moat (same seed → bit-identical) + **AVX2 bulk fill** (Philox 8-block / Threefry 4-block SoA, bit-identical to scalar — gated, 34061 asrt). ⭐⭐ **FULL CRUSH vs NumPy AND MATLAB R2026a — every same-generator WIN, NO losses** (ns/u64): PCG64-DXSM 1.54×N · SFC64 2.91×N · Philox 1.97 (1.37×N / 3.13×M) · Threefry 2.05 (1.81×M) · MT19937 1.46 (1.52×N / 2.37×M) · xoshiro256** 0.67 + splitmix 0.38 = fastest (2.5–4.4× MATLAB simdTwister 1.69). [SIMD-vectorizing Philox/Threefry was the lever that flipped the two former losses.] **▶ v12-f + v12-g SHIPPED 2026-06-24** (`samplers.hpp`/`ziggurat.hpp` + `qmc.hpp`/`chacha.hpp`; full stats suite **302539 asrt / 31 cases GREEN on win-debug + win-asan (0 ASan errors) + win-shipping; win-tidy clean + guards**). **v12-f** Ziggurat normal/exp · Marsaglia-Tsang gamma · gamma-ratio beta · χ² · Knuth+PTRS Poisson · BINV+BTPE binomial · geometric · Vose alias · reservoir (moments + empirical-CDF-vs-special-fn-CDF + determinism gates). ⭐⭐ **CRUSHES MATLAB-1T 8/8 (1.26×–277×)**; **beats NumPy 7/8** (normal 2.10×/gamma 1.47×/beta 1.50×/poisson 1.38×,1.41×/**binomial 1.76×,1.20×**), exp ~parity (DXSM-quality tradeoff). ⭐ user-directed chase closed the 2 NumPy binomial losses via a stateful **`BinomialSampler`** (precompute BTPE/BINV setup once = NumPy's `binomial_t` pattern; bit-identical to free `binomial()`, gated 20000 checks) — root-caused generator-independent by a decomposition probe; exp gap is the higher-quality DXSM generator (faster gens made it worse via register pressure), not chased. **v12-g** Sobol (Joe-Kuo dirs, dims≤21, bit-vs-scipy) · Halton · rank-1 lattice · LHS · ChaCha20 (RFC 8439 KAT). ⭐ **FIXED a real `binomial_inversion` INFINITE-LOOP** (not reflection-aware ⇒ p>0.5,large-n underflows q^n → spins forever; hung win-debug; reflect internally like BTPE — boundary-adversary, only {500,0.95} hit it) + 7 win-tidy violations cleared (topology.cpp local/static-const naming + nested ternary; test kN→n + multi-decl split). **▶ v12-h + v12-i SHIPPED 2026-06-24** (`distribution.hpp` framework + `continuous.hpp` 25 + `discrete.hpp` 12; umbrella + tests wired). The **`Distribution<T>`** surface (CRTP base + C++20 concept): pdf/logpdf/cdf/logcdf/sf/logsf/ppf/isf/rvs + mean/var/std/skew/kurt/median/entropy (+mgf/fit where they exist), f32/f64. **v12-h** 25 continuous (normal…vonMises/Rice) — CDFs ride hesap-special erf/gammainc/betainc+inverses, rvs rides v12-f. **v12-i** 12 discrete (Bernoulli…logarithmic). **GOLD-STANDARD GATED vs scipy.stats: continuous 650/0 (pdf/cdf/sf <1e-9, ppf <1e-7, moments+entropy), discrete 304/0 (pmf/cdf <1e-9, integer ppf exact).** 4-config Windows DoD GREEN (debug/asan/shipping **309570 asrt / 40 cases**, 0 ASan errors; win-tidy clean — headers tidy-clean first try; 5 guards). ⭐⭐ **PERF (ns/elem vs scipy AND MATLAB-1T): pdf/pmf CRUSH everywhere** — normal.pdf 5.7×scipy/2.8×MATLAB · studentt.pdf 2.05×/2.3× · gamma.pdf 1.4×/8.2× · poisson.pmf 2.4×/10.7× · binomial.pmf 2.1×/13.9×; most cdfs win. ⭐⭐⭐ **THEN THE betainc/gammainc CRUSH (user: "no losses, crush every library"): the 4 incomplete-beta/gamma-bound losses all FLIPPED to wins** — (1) **amortise lgamma** (fixed-(a,b) distributions precompute lgamma/lbeta ONCE via new cached `gammainc_p/q(a,x,gln)`/`betainc(a,b,x,lbeta)`/`betainc_inv(…,lbeta)` overloads; scipy's frozen ufunc recomputes per-element) — also crushed the pdfs (gamma.pdf 3.4× · beta.pdf 4.1× · studentt.pdf 5.84×); (2) **looser CF tolerance** (kCfEps=8·machine-ε, gate is 1e-12) — gammainc 65→43ns; (3) **binomial.cdf direct pmf-sum** (shorter tail, n≤200) — 112→7ns; (4) **studentt.ppf = Hill AS-396 direct quantile + 1 Halley** (what scipy's stdtrit does) — 446→130ns; (5) **normal.ppf = Wichura AS-241 `ndtri`** (new reusable probit in hesap-special erf.hpp — pure rational, no iteration/erf, full f64, coeffs verified vs R qnorm.c) — 21.6→2.7ns (0.95→**6.48×**); also speeds the studentt Hill init (→1.6×) + lognormal/halfnormal ppf. **FINAL all-peers board: vs scipy 16/16 WIN (1.04×–6.48×, NO losses, NO parity); vs MATLAB-1T 16/16 WIN (to 20×).** Accuracy preserved (continuous 650/0, discrete 304/0, **hesap-special suite still 402081/37 <1e-12**). Determinism moat on rvs. **▶ v12-j SHIPPED 2026-06-24** (`heavy_tail.hpp`, 8 distributions + the α-stable sampler): GEV (extreme-value) · GPD (gen-Pareto) · Lévy (stable α=½) · BetaPrime · NoncentralChiSquared (cdf via shipped Marcum-Q) · **SkewNormal** (cdf = Φ−2·Owen's-T, via tan-sub + composite Gauss-Legendre) · **NoncentralT** (Lenth AS-243 cdf series + pdf series) · **NoncentralF** (Poisson-mixture-of-betas series) · **StableSampler** (Chambers-Mallows-Stuck CMS, Nolan S1; gated by special cases α=2→normal / α=1→Cauchy + determinism). **GATED vs scipy.stats 208/0** (genextreme/genpareto/levy/betaprime/ncx2/skewnorm/nct/ncf; pdf/cdf/sf <1e-9, ppf <1e-6, moments+entropy). Full suite a→j: win-debug + win-shipping **317795 asrt / 44 cases**, win-tidy clean (asan running). ⚠ general-α stable pdf/cdf (Zolotarev quadrature — scipy itself does it numerically) is the one genuinely-deferred piece. NEXT = **v12-k** (multivariate: MVN/MVt/Dirichlet/Wishart/LKJ/multinomial). **▶ v12-k SHIPPED 2026-06-25** (`multivariate.hpp`, 7 distributions over the shipped Cholesky): MultivariateNormal · MultivariateT · Dirichlet · Wishart · InverseWishart · LKJ (correlation) · Multinomial. Factor Σ via `dense::factor_cholesky` in the ctor (**new acyclic `stats→hesap-dense` edge** — dense never references stats; confirmed) → L copied to a flat raw `Array<T>` for the tight raw-f64 hot path (ADR-0078). **GATED vs scipy.stats <1e-9** (MVN/MVt/Dirichlet/Wishart/InverseWishart/Multinomial — inv-Wishart df/scale convention verified numerically; 1-indexed Bartlett verified by the E[W]=νV moment check) + **the analytic LKJ p=2 marginal** (no scipy peer) + the **cross-thread determinism moat** (per-sample Threefry streams, partition-invariant {1,4,16}). `[v12-k]` 50 asrt / 8 cases; full stats suite **317845 / 52**; **4-config Windows DoD GREEN** (debug + asan 0-err + shipping LTCG + tidy) + 5 guards + linux-gcc. ⭐⭐ **CRUSH BOARD — all 7 crush every applicable peer** (Cerid ns vs scipy/numpy + MATLAB-1T): MVN logpdf **2.35× scipy / 3.17× MATLAB** · MVN rvs **4.07× numpy / 2.30× mvnrnd** (fastest gen; Threefry-moat path 0.87× = the cross-thread-determinism feature peers lack) · MVt logpdf **1.44× / 1.63×** (was parity → fixed: `log1p`→`log`, accuracy-safe since 1+q/ν≥1 has no cancellation) · Dirichlet logpdf **3.48× scipy** · Multinomial logpmf **2.36× / 2.89×** · Wishart rvs **7.17× scipy / 22.4× wishrnd** · InverseWishart rvs **4.10× / 16.9×** · LKJ rvs = no fair peer (Stan only). The crush lever = ctor-amortized log-Γ_p/lgamma/Cholesky (scipy's frozen object recomputes per-call). **▶ v12-l SHIPPED 2026-06-27** (`log_density_grad.hpp` + `multivariate.hpp` ∇ methods): analytic ∂logp/∂x + ∂logp/∂θ for every distribution (25+12+8+MVN/MVt; the HMC/NUTS + MLE enabler). Exp-family suff-stats yield **algorithmic crush — Normal ~888,000×, Gamma ~8,700× vs JAX** (via O(1)-per-leapfrog precompute); StudentT O(N)-irreducible, **near-parity 1.14× vs JAX XLA** (not a crush, but competitive). FD-checked (348 asrt). **▶ v12-m SHIPPED 2026-06-27** (`descriptive.hpp`): moments (mean/var/stddev/skew/kurt, scipy conventions, +robust: MAD/trimmed) · quantiles (9 R Hyndman-Fan types + Harrell-Davis) · ECDF · histogram bin-rules · weighted · covariance/correlation matrices. Gated vs scipy/numpy/MATLAB <1e-9 (82 asrt). Full stats suite **318275 / 105**; linux-gcc-release green, Windows DoD pending. **▶ v12-n DONE 2026-06-28** (`hypothesis.hpp`, the FULL classical hypothesis-test suite — every test in the table): parametric (t-test 1-sample/independent/Welch/paired, one-way ANOVA, Bartlett, Levene/Brown-Forsythe); nonparametric (Mann-Whitney U, Wilcoxon signed-rank, Kruskal-Wallis, Friedman); correlation (Pearson, Spearman, Kendall tau-b, distance correlation); goodness-of-fit (KS 1-sample/2-sample, Anderson-Darling, Shapiro-Wilk via Royston AS R94, Jarque-Bera); categorical (chi-square contingency + Yates, Fisher exact, McNemar); multiple-comparison (Tukey HSD via studentized-range Gauss-Hermite x Gauss-Laguerre integral, Holm, Benjamini-Hochberg FDR); effect sizes (Cohen's d, eta-squared). PLUS every remaining variant (now shipped): z-test · 2-way + repeated-measures ANOVA · sign · Mood · Cramér-von-Mises · D'Agostino-K² · Lilliefors · G-test · Bonferroni · Scheffé · Dunnett (equicorrelated-MVt via the same Gauss-Hermite×Gauss-Laguerre integral) · Games-Howell · Cramér's V. **Every test in the table, 121 asrt vs scipy/statsmodels — CRUSHES scipy/R/MATLAB** (37×-1142× per-call); new acyclic edge `stats→quadrature` (studentized-range nodes). Honest scope: Mood no-tie variance (scipy adds a Mielke tie correction for tied samples) · Dunnett balanced/equal-treatment-size · CvM asymptotic p (scipy adds a ~2e-4 finite-n correction). Full stats suite **318396 / 116**. **▶ v12-o DONE 2026-06-28** (`resampling.hpp` + `resampling_parallel.hpp`, the FULL resampling suite): bootstrap (percentile/basic/BCa/studentized) · block bootstrap (moving-block) · jackknife (+delete-d) · exact+MC permutation · CV (`kfold_indices`, k=n→LOO) · **parallel over crd-jobs, bit-identical to serial = the determinism moat under threading** (new acyclic stats→jobs edge). 52 asrt: deterministic core (jackknife/CI-math/exact-permutation) bit-for-bit vs scipy/statsmodels, RNG bootstrap CI MC-gated vs scipy.stats.bootstrap, same-seed + parallel==serial bit-identity. ⭐⭐ **FULL CRUSH per-thread AND parallel** (B=100000 percentile-CI-of-mean): serial 22.8ms = **2.08× scipy / 21.4× R / 13.8× MATLAB**; parallel(32c) 9.4ms = 5.05× / 51.9× / 33.4×. The per-thread crush (user: "push serial until it beats scipy") = killing the `% n` modulo (Lemire `bounded`) then Threefry4x64→**Philox4x32 + u32 Lemire** (indices fit in 32 bits) — a 0.74× serial LOSS → 2.08× WIN, moat preserved. Full stats suite **318448 / 119**. **▶ v12-p DONE 2026-06-28** (`kde.hpp`/`robust.hpp`/`cov_robust.hpp`/`streaming.hpp`/`tdigest.hpp`): KDE Gaussian/Epanechnikov (Scott/Silverman/rule-of-thumb/LOO-CV bw; Gaussian bit-matches scipy.stats.gaussian_kde) · robust (Theil-Sen/Hodges-Lehmann/Huber-M/Tukey-M/Huber-proposal-2) · covariance (Ledoit-Wolf/OAS/exact-MCD) · streaming (Welford/P²/t-digest/HyperLogLog/count-min). 49 asrt: deterministic estimators bit-for-bit vs scipy/statsmodels/sklearn/numpy, probabilistic sketches within-accuracy. ⭐⭐ **CRUSH (ns/call): kde 5872× scipy · huber 503× statsmodels · oas 437× sklearn · ledoit_wolf 257× · theil_sen 6.9×**. 2 fixes via python-replication: RLM scale = center-0 MAD `median(|resid|)/0.6745`; Huber proposal-2 uses N−1 df in the gamma term. Full stats suite **318497 / 124**. **▶ v12-q DONE 2026-06-28** (`mcmc_diagnostics.hpp` + `mcmc.hpp`): MCMC/Bayesian — Metropolis/adaptive-Haario/Gibbs/HMC/**NUTS**(recursive tree-doubling + U-turn + **dual-averaging** = the full Stan algorithm)/slice/SMC + diagnostics (rank-normalized R-hat / Geyer bulk-ESS / autocorr / Geweke). 36 asrt: **diagnostics gated BIT-FOR-BIT vs ArviZ** (4 formula bugs caught by reading their `arviz_stats` backend source — rank-denom `+1/4`, R-hat=max(bulk,folded), ESS Geyer `max_t=t−2`); samplers recover known targets + the determinism moat (same-seed bit-identical) + bit-exact leapfrog. ⭐⭐ **CRUSH (effective-samples/sec, the table metric): Cerid NUTS 907,142 ess/s vs PyMC 8,711 = 104×** (87× samples/s); HMC 12.7M ess/s. RNG = Threefry (moat); installed arviz+pymc in WSL. Full stats suite **318533 / 128**. **▶ v12-r DONE 2026-06-28** (`regression.hpp`): Regression/GLM/multivariate — 9 groups (OLS/WLS/GLS · ridge/lasso/elastic-net · GLM-IRLS logistic/Poisson/gamma · robust-Huber/quantile/**RANSAC** · PCA/LDA/QDA/**factor-analysis**), all riding the shipped `dense::lstsq`/`pinv`/`eig_sym` (**SANITY 8 reuse — zero reimplementation**; the `stats→dense` edge existed from v12-k). 58 assertions **all gold-standard gated vs statsmodels/sklearn** (OLS coef+R²+SE, LDA/QDA exact-integer predictions, RANSAC same-seed bit-identical = determinism moat). ⭐⭐ **CRUSH vs sklearn (fits/sec, native C++ over the shipped factors vs Python interpreter+numpy-dispatch overhead): Ridge 47.2× · PCA 23.2× · OLS 16.9× · Lasso 16.7× · GLM-logit 2.7×.** Benches `bench_regression.{cpp,py}`. Full stats suite **318591 / 135**. **v12 arc a→r COMPLETE.** **▶ v12-z ✅ DONE (close-out, 2026-06-28) ⇒ THE v12 STATISTICS ARC a→z IS COMPLETE.** (1) **Deferred v11 spectral CIs back-wired**: multitaper PSD **χ² CI** (`multitaper_psd_ci` + `dof_out`; ν=2K/Thomson ν(f); χ²_{ν,q}=2·`special::gammainc_p_inv`(ν/2,q)) gated vs an **INDEPENDENT scipy `chi2.ppf`** ref (advisor: a self-consistent ν=K ref passes green even if wrong) + the frequency-constant invariant; AR-PSD **asymptotic-normal CI** (`ar_psd_ci`, Berk Var{log Ŝ}≈2p/N ⇒ Ŝ·exp(±z·√(2p/N)), z=ndtri) — **no clean scipy/MATLAB peer exists (honest "no gold standard" case), analytic-gated + documented**, the user chose implement-not-skip. (2) **CLI** `hesap.stats.{describe,ttest_1samp}.f64` + `hesap.special.{gamma,erf}.f64` (curated agent subset via `CRD_HESAP_CLI_REGISTER_MODULE`+`cli_anchor.hpp`). (3) **System docs**: `hesap-special.md` created + `hesap-stats.md` rewritten. (4) **ADR-0094** created + indexed. 17 asrt; dsp 27076 / special 402091 / stats 318601 GREEN. **PENDING = user's process only: Windows 4-config DoD + 18-config CI + COMMIT (v12-q + v12-r + v12-z uncommitted; v12-n/o/p committed; v11 `768d8d9`; crd-math `76f297a`/`1891fbc`).**

**▶▶ (historical) Active: v10 — FFT, new module `crd-hesap-fft`. Ratified bar = BEAT MKL; scope a→h** (memory `project_v10_fft_plan`; phase doc v10 block; session `2026-06-13-v10a-fft-substrate.md`). **v10-a/b (1D complex) + v10-d (real FFT) SHIPPED this round (UNCOMMITTED).** `FftPlan<T>` = deterministic-plan Stockham autosort (no runtime measurement, shared twiddle table ⇒ cross-thread bit-identical) + **genfft-lite codelet generator** (`scripts/gen_fft_codelets.py`: CSE'd straight-line leaf + radix-L twiddle codelets, numpy-self-checked, **register-pressure-scheduled** loads-late/stores-early). Correctness gate = brute-force O(N²) DFT (NOT round-trip — the odeint-d4 trap) + oracle cross-checks. **v10-d** `RealFftPlan<T>` rfft/irfft via half-size complex + Hermitian recombine. ⭐ **THE 1D-vs-MKL GRIND (this session, the user pushing iterate-and-attack):** 4 measured wins — interleave fold (+1.7× large-N) · scheduler flips radix-8 from −17%→win · radix-32 + mixed-radix size-aware planner (+25% @L2) · lifetime-aware scheduler (+5–11% @L1/L2) → **~0.35×→~0.45× MKL, BEATS PocketFFT (numpy/scipy default) EVERYWHERE, 1e-15, deterministic**. ⭐⭐ **The structural+source space is now FULLY MAPPED + MEASURED** (4 research rounds: FFTW genfft / Spiral / asm / ryg blog): every alternative is measured-SLOWER — bigger-radix-over-k>32 (spills), six-step transpose (loses), cache-oblivious recursion (loses 3.6×), batched four-step (loses), across-radix (refuted), in-place DIT (refuted 2.6–3.3× — Stockham's no-bit-reversal beats 1× footprint here), FMA (compiler already fuses). **Honest verdict: Stockham + the 4 wins is the structural-and-source OPTIMUM on this AVX2 box (~0.45× MKL); the residual ~2.2× is the genfft/Spiral scheduler-as-a-compiler-pass = a dedicated multi-session cooled-box sub-project, NOT a source tweak (and sub-parity alone).** **v10-d real FFT CRUSHES numpy/scipy/PocketFFT rfft** (~2× Hermitian on the PocketFFT-beating core). Gate **53/11 win-debug + win-asan**. ⚠ 14900K thermally throttles after sustained FFT bench sweeps (CLAUDE.md hazard) — rel-to-MKL is the trustworthy axis; cool box for the scheduler-project measurements. **▶ v10-g NUFFT SHIPPED 2026-06-14 (session `2026-06-14-v10g-nufft.md`)** — `nufft.hpp` `NufftPlan<T>` type-1/2 (ES kernel, σ=2, spread→v10-b-FFT→deconvolve; gate = direct O(NM) nonuniform DFT, spreader run-twice bit-identical). ⭐ **vs FINUFFT** (built from src, 1 thread, EXECUTE-only, Cerid width+2 ⇒ **3× MORE accurate**): naive 0.5× → wrap-split+memset + multi-accumulator interp → **WINS small/mid (4096 T1 1.99× / 16384 T2 1.73×), PARITY at 1M (T2 1.14×), loses only FFT-bound n=2^18** (62–91% FFT = the SAME v10-b FFT-engine deficit, deferred; FINUFFT there = FFTW_ESTIMATE). The NUFFT-specific spread/interp = competitive-to-winning. ⚠ bin-sort MEASURED-REGRESSED (36MB L3 hides scatter) → reverted. HONEST: serial run-twice determinism (NOT a built {1..16} moat — parallel owner-per-subgrid spread designed, not implemented); EXECUTE-only timing. Suite **66/16 on 4 configs (debug+asan+tidy+shipping)** + fixed 8 pre-existing tidy errors in test_fft.cpp. **▶ v10-f DCT/DST SHIPPED 2026-06-14** (same session log): `dct.hpp` `DctPlan<T>` DCT-II/III + DST-II/III (Makhoul O(N log N) over FFT; every formula verified vs scipy.fft in `scripts/dct_research.py` before porting; forward dct2/dst2 on the v10-d REAL FFT = half work). GATE = direct O(N²) sum (NOT round-trip) + 2N round-trip + determinism; 43/4, suite **109/20 on 4 configs**. ⭐ **DCT-II shootout: BEATS PocketFFT (numpy/scipy backend) at EVERY size** (1.0–1.7×; real-FFT path flipped large-N 0.86→1.25×); vs FFTW 0.65× (win @32768) = the SAME FFT-kernel wall (FFTW/MKL ahead via tuned kernels, not a DCT gap). ⭐⭐ **THE MKL-1D GRIND — TWO REAL WINS 2026-06-14** (user-mandated "full parity, keep grinding"; session `2026-06-14-fft-lever-d-block-four-step.md`, the Part-14 handoff is the current state — the Part 1–5 "reverted, ~0.45× ceiling, pivot to breadth" verdict was SUPERSEDED in the same session after the user supplied the `hpk::fft` paper proving MKL is beatable 1.6× in C++ on AVX2): **(1) SPLIT-RADIX (2/4) codelets** (`gen_fft_codelets.py` + `codelets.hpp`; 22% fewer real-muls, numpy-self-checked) = **+3–13% small/mid**; **(2) FOUR-STEP RESURRECTION for n≥2¹⁹** — NT-store **blocked transpose (25.7 GB/s**, 3× the old scalar 8.1, via `_mm_stream_pd` 64 B-aligned crd buffers) + **nested radix-4/8 batched sub-FFT** (hoisted ctor sub-plans; the radix-4 NESTED-twiddle bug for bit-reversed input was gate-caught) + 1 MB blocks ⇒ **large-N 0.32–0.47× → ~0.43–0.75× MKL, beats the direct path across all 512K+**. Still beats PocketFFT/scipy/numpy everywhere, 1e-15, deterministic; `[fft]` gate 16/16. **The remaining parity gap = the strided phase-1/2 gather floor** (54% @8M, STRIDED-memory-bound ≈14 GB/s — SIMD-mul measured a WASH; the four-step must gather full columns). **NEXT BUILD = the blocked-tile (B×B) NT gather/transpose woven into phase-1/2** (the transbw 25.7 GB/s pattern → ~parity; delicate, fresh-context). Measured-DEAD (don't re-try): naive four-step/six-step · cache-oblivious recursion · split-radix-as-latency-lever · radix-64 · group-ILP · SIMD-twiddle-mul. **PENDING USER: the v10-a/b/d/g/f + the two wins COMMIT** (proposed message in the session log tail); DoD re-confirm split-radix/kBudget; extend the oracle test to gate the 2¹⁹ crossover band. **⭐ AVX2 LARGE-N ARCHAEOLOGY CAMPAIGN CLOSED 2026-06-16 — FOUR banked wins** (larger-factor-first split · f32 NT scatter · f32 bb-axis SIMD twiddle · f64 bb-axis SIMD twiddle): **f64 8M 0.76→0.84× · f32 8M 0.61→0.78× MKL** (dossier `docs/research/fft-mkl-crush.md`, FINAL SUMMARY). 3 wins committed (`d5ae96d`,`d0f2484`); **f64 twiddle UNCOMMITTED** (working-tree `fft.hpp` +65 lines — PENDING USER COMMIT). Local patching is closed; the residual sub-FFT gap is bound-analysis-proven to need genfft-class codegen. **⭐ FORK A — Generated Codelet / Planner Project OPENED** (`docs/design/hesap_fft_generated_codelets.md`; generator `build/gen_subfft.py`): **M0 ✅ + M1 ✅ 2026-06-16** — generated scheduled split-radix BEATS the engine at N=32 (1.98×)/N=64 (1.24×) but the **CROSSOVER at N≈64** (N=1024 = 21996 spills / 574 s compile / 0.92×) ⇒ full straight-line REJECTED; **4096 path = Path B hierarchical 64×64** (generated-64 leaf + bb-axis twiddle + 64×64 transpose). **M2 DONE 2026-06-17 (`docs/sessions/2026-06-17-fft-m2-hierarchical-64x64.md`): the hierarchical 64×64 composition is PROVED.** Variant B (fused twiddle+transpose into stage-1 stores) = **sub-FFT 1.240×** over `execute_batched(4096,16)` (machine-eps); composed into the engine four-step behind `CRD_FFT_M2_HIER` (gated OFF, default TU byte-identical): **full-16M ~1.10× (both sub-FFTs 4096 ⇒ ~51% accelerated, CLEARS the 1.05× gate, 3/3 interleaved, 1.1e-15) · full-8M ~1.04× (only n1=4096 = 27% accelerated, below the gate — the design-doc "~46%" conflated both sub-FFTs).** Variant A (explicit transpose) lost (0.740×). ⚠ the advisor caught two over-reads (a first "16M no-show" was an unverified-correctness noise sample; the bbuf-pressure mechanism was contradicted by identical 8M/16M working sets). **M3 DONE 2026-06-17 (`docs/sessions/2026-06-17-fft-m3-hierarchical-2048.md`): the 2048 = 64×32 hierarchical sub-FFT (Choice B, 1.260× isolated) accelerates the 8M four-step's n2=2048 sub-FFT too ⇒ BOTH 8M sub-FFTs hierarchical (~51%).** Wired beside the 4096 path (same gate): **full-8M 1.13–1.17× engine → ~0.94× MKL (≥1.05× gate CLEARED, matches the prompt's "8M ~0.92–0.94×") · 4M 1.13–1.24× → ~0.95–0.99× MKL (near parity) · 16M unchanged-by-construction (byte-identical 4096 codelet) · all machine-eps 1.1–1.2e-15.** Three independent measurements agree. ⚠ report the CANONICAL ×MKL (~0.94), not the flattering m3_full probe (~1.04). **M4 DONE (gate flipped ON by default) 2026-06-17 (`docs/sessions/2026-06-17-fft-m4-enable-default.md`):** the M2(4096=64×64)+M3(2048=64×32) hier sub-FFTs are now the DEFAULT f64 forward path (`#ifndef CRD_FFT_DISABLE_HIER`; inverse/f32 → radix-8 fallback; oracle test extended to 2^23 to gate the 4096 path). Verified: 4 win-configs FFT-scope + ASan-clean + LTCG + clang-tidy + gcc; **`ctest --preset win-debug` GREEN 3935/3935 incl. guards**; perf **8M 1.13–1.21× → ~0.90–0.94× MKL, 4M ~0.99× near-parity, no regression, machine-eps**. ⚠⚠ hit 2 PRE-EXISTING env landmines (C1853 stale-PCH from an MSVC update ⇒ `Get-ChildItem build\win-* -Recurse -Filter cmake_pch.cxx.pch | Remove-Item` + `per-slice-check -Reconfigure`; `ctest --preset` from plain PowerShell lacks `dumpbin` ⇒ the simd-emission guard spuriously fails, use `scripts/run-ctest.bat`). **NOT committed — clear stale PCHs → finish the multi-config sweep → commit.** ⭐ **M4 `251bd79` + M5 `d602c30` + toolchain-cleanup `aeb0086` COMMITTED; full 4-config DoD GREEN (win-debug/asan 3935/3935, win-shipping 3848/3848, win-tidy clean).** M5 = hier 1024=32×32 added to the default hier path (a real KEEP: engine ~1.15–1.20× on 1M/2M vs radix-8). ⚠ **HONESTY CORRECTION (M6 Phase 0): the M5 "2M ~0.99× near-parity / 1M 0.78×" used the `m3_full` harness which flatters Cerid ~20% vs the CANONICAL `bench_fft_vs_refs`. TRUE canonical board: 1M 0.60× · 2M 0.75× · 4M 1.04× (BEATS MKL) · 8M 0.91× · 16M ~0.90×.** ⭐ **M6 Phase 0+1 DONE (profile-first): the sub-FFTs are hier-optimized; the residual 8M/16M loss is MOVEMENT — the four-step GATHER is the biggest lever (~34–35% combined); 1M is MKL-near-optimal (L2-resident, hard).** **NEXT = M6 front choice (Phase 3): Front C 8M/16M gather fusion (highest ROI, but the known-hard strided-gather wall) vs bank the result + pivot to f32 (Vec8f hier port, biggest untouched lever, f32 ~0.78×) · inverse hier · planner cost-model · v10-e/c.** ⚠⚠ ONLY use bench_fft_vs_refs for ×MKL (m3_full flatters). **▶ M7 f32 Vec8f hier port + M8 inverse + M9 1M/2M (2026-06-18, all UNCOMMITTED, engine working-tree):** M7 wired the f32 hier (Vec8f, same 1024/2048/4096 decompositions) + default-enabled + 4-config DoD. M8 inverse hier = REVERTED (conjugation-wrapper regressed; radix-8 inverse already efficient). **⭐⭐ M9 found a REAL BUG: the M7 f32 DISPATCH gate was `#ifdef CRD_FFT_F32_HIER` (never-defined) — a `replace_all` indentation-miss left it OFF while the ctor was ON ⇒ f32 silently ran radix-8; every "f32 doesn't compose" measurement was radix-8-vs-radix-8.** FIXED (dispatch gate → `#ifndef CRD_FFT_DISABLE_F32_HIER`, matches ctor). **⭐ NOW f32 hier COMPOSES (interleaved CRD-vs-CRD, robust): 1M 1.33× · 2M 1.26× · 8M 1.25× vs radix-8; ×MKL (interleaved CRD-vs-MKL): 1M 0.75 · 2M 0.81 · 4M 0.97 · 8M 0.99 = PARITY at 4M/8M.** VERDICT USE NOW. ⚠ also overturned: the whole-session ×MKL dashboard (M2–M7) was cross-run best-of-reps = NOT canonical — only same-process-adjacent INTERLEAVED A/B is trustworthy. f64 hier composes too (1M/2M 1.22×, robust). M9 DoD re-running (f32 path changed radix-8→hier). NEXT M10: f32/f64 1M/2M residual (MKL's small-N cache efficiency — the genuinely hard part). [[project_v10_fft_plan]] Parts 33–39.

**Open follow-ons (post-v5/v6, not blocking — track in `docs/debt.md`):** CI 18-config sweep on the WIP; `docs/systems/hesap-direct.md` system doc; ADR-0065 §27 D(direct) lock; all-families `{1..16}` moat audit; raefsky3 DELAYED-PIVOT frontier (the one matrix within-front pivoting couldn't take to full f64).

---

## Coming up next (Phase 3.1 eylem — ⏸ PAUSED at v1b)

Phase 3.1 eylem is paused at v1b close per the ADR-0076 §12 sequencing pivot (2026-05-11): `crd-geometry` ships FULL before eylem v1c resumes, so eylem v1c+ consumes geometry from day 1 (no deferred-refactor debt). Resume order after the hesap cluster: v1c broadphase → v1c-sensor → v1d narrowphase + filter + callback + mesh + hf → v1e SI solver + material → v1f joints + articulation-filter + fields → v1g islands + contactmodify → v1h scene queries → v1i character controller → v1j snapshot/replay → v1k sandbox → v1l close. Full slice plan in `docs/phases/phase-3.1-eylem.md`.

---

## Active detour

_none — D-001 closed 2026-05-07; D-002 (concurrent containers) 2026-05-12; D-003 (crd-perf) + D-006 (crd-time) 2026-05-15._

> When a detour opens, this section names it and the main roadmap pauses until it closes. Detour files: `docs/detours/D-NNN-<slug>.md`. Queue rules: `docs/detours/README.md`.

---

## Last shipped milestone

**2026-06-07 — v6 (sparse eigenvalue) CLOSED + committed (`4fd0b84`).** New module `crd-hesap-eigen`: matrix-free Lanczos · thick-restart Lanczos (≡IRLM) · Arnoldi/Krylov-Schur (≡IRAM) · shift-invert · LOBPCG (+generalized +preconditioned) · Jacobi-Davidson · FEAST · IRLBA sparse SVD; CLI `hesap.eigen.*`; the `{1..16}` determinism moat on every method. ADR-0089 + `docs/systems/hesap-eigen.md`. Honest verdict: AMG-LOBPCG crushes direct ARPACK 9.5× wall + 40× mem, parity vs PRIMME; sparse SVD competitive at matched accuracy + moat.

**2026-06-05 — v5 (sparse direct) CLOSED + committed (`f0ae6db`).** The full family: v5a Cholesky · v5b LU · v5c QR · v5d LDLᵀ · v5e HSS/BLR · v5f mixed-precision IR + GMRES-IR + within-front partial pivoting — all with the cross-thread determinism moat. SPD Cholesky beats CHOLMOD (hood/ldoor) via a profiled serial-symbolic fix; LU beats MUMPS on af23560; honest losses recorded where MUMPS/UMFPACK win. Memory `project_v5f_mixed_precision`, `project_symbolic_is_the_cholmod_gap`.

For the full hesap story (v0 BLAS → v1/v2 sparse substrate + reorderings → v3 dense eig/SVD → v4 iterative + AMG → v5 sparse-direct → v6 eigen → v7 opt), see `docs/phases/phase-3.1.6-hesap.md` and the session logs.

### Recent slice history (one line per cluster — full detail in the linked session log / phase doc)

- **v10 (FFT, `crd-hesap-fft`) — ▶ IN PROGRESS (a/b/d/g/f + 2 MKL-grind wins + small-N engine crush, UNCOMMITTED)** — deterministic-plan Stockham + **split-radix** genfft-lite codelets + **four-step resurrection** (NT-store blocked transpose + nested radix-4/8 batched sub-FFT, n≥2¹⁹) + real FFT + NUFFT + DCT/DST; **beats PocketFFT/scipy/numpy everywhere (1e-15)**, **large-N ~0.43–0.75× MKL**; rfft/DCT crush scipy. ⭐ **SMALL-N CODELETS WIRED 2026-06-15** (`small_n_codelets.hpp`, AoS lane-trick N∈{8,16,32} f64 fwd+inv, gated machine-eps): `execute()` single-call = **parity with MKL + 1.5–2.5× faster than the prior SoA leaf** (throughput-by-nature construction; latency-bound single-call = parity, NOT a crush); **`execute_batched` N=8 even-batch = 1.49–2.25× over MKL-batched** (re-verified 2026-06-15 gated 2.44e-16; batch-size dependent). ⭐ **N=16 batched CRUSH SHIPPED 2026-06-15** (`small_n_batched16_f64`): extended the over-2 atom to N=16, **0.30× → 1.36× on the shipped engine** (probe 1.40–1.53× for cache-resident batch b≤192; guarded `n·b≤2048`, SoA fallback beyond the L1 cliff). Research-grounded (hpkfft: single-transform L1 mid-band is a loss even for the best C++ FFT — 0.82× — but the BATCHED regime wins +20% over vendor; `docs/research/fft-mkl-crush.md` §16). Single-transform N≤32 = parity; mid-band single = the recursive fused-codelet generator (person-weeks, four-step ruled out — double-capped 0.47×). N=32/64 batched next (block-transpose). N=16/32 batched = strided-gather wall (next, v10-e). Suite 126/21; win-debug/asan/shipping/tidy + gcc-strict clean. Large-N parity gap = the converged genfft engine (person-weeks). `docs/sessions/2026-06-15-fft-small-n-engine-crush.md`; memory `project_v10_fft_plan`. ⭐ **2026-06-16: AVX2 large-N archaeology CLOSED (4 banked wins, f64 8M 0.84× / f32 8M 0.78× MKL; f64 twiddle PENDING COMMIT) + Fork A Generated-Codelet Project OPENED — M0/M1 done, crossover at N≈64, 4096 = Path B hierarchical 64×64, NEXT = M2.** `docs/sessions/2026-06-16-fft-generated-codelet-m0-m1.md`; design `docs/design/hesap_fft_generated_codelets.md`; dossier C-22/C-23.
- **v9 (ODE/DAE, `crd-hesap-ode`) — ✅ COMPLETE a→z 2026-06-13, awaiting the user's commit** — a…h + j(sparse) committed `b578f66`; the i/j-Krylov/k/l/z batch (IMEX ARK3/4/5 · matrix-free Krylov SPGMR · forward+adjoint sensitivities · Pryce structural index + mechanical index reduction · CLI) closed this round; suite 577/67 on debug+asan+shipping; ADR-0091. `docs/sessions/2026-06-13-v9-imex-krylov-sens-dae-batch.md`.
- **v7 (optimisation, `crd-hesap-opt` + `crd-hesap-stats`) — ✅ COMPLETE a→z + FULL CRUSH 2026-06-11, awaiting the user's commit** — a→e committed `b261478`; f→z closed 2026-06-10 in one run (Philox v12-pull · QP/LP/conic/NLP/modeling · the DFO trio + 3 NLopt-bit-exact Powell ports (8868/0) · CMA-ES/global · MIP · CLI + gold scoreboard); the crush pass (Ruiz ADMM 54→31 · Powell BEATS scipy · active CMA BEATS pycma on ros5 · BH parity · torch 12-digit trajectory match) closed every gap 2026-06-11; ADR-0090. `docs/phases/phase-3.1.6-hesap.md`.
- **v6 (sparse eigenvalue, `crd-hesap-eigen`) ✅ committed `4fd0b84`** — Lanczos/Arnoldi/LOBPCG/JD/FEAST/IRLBA + CLI + moat; ADR-0089.
- **v5 (sparse direct) ✅ committed `f0ae6db`** — Cholesky/LU/QR/LDLᵀ/HSS-BLR/mixed-IR family + moat.
- **v4 (iterative solvers + preconditioners + AMG) ✅** — `docs/sessions/2026-05-27-hesap-v4z-close.md`; ADR-0065 §26.
- **v3 (dense eig + SVD + least-squares) ✅** — symmetric + non-symmetric (balance/Hessenberg/Francis/AED) + SVD + lstsq/pinv/NNLS/TLS; ADR-0065 §24.
- **v0 (dense BLAS L1/L2/L3 + microkernels) ✅** — ADR-0082 (intrinsics-via-Vec8f microkernel; asm deferred).
- **2026-05-19 — Phase 3.1.7 `crd-geometry` substrate CLOSED** — all 11 sub-modules (primitives→bvh→convex→mesh→spatial→polygon→mesh-processing→delaunay→gpu-lbvh→decomposition→shader-helpers); ADR-0076. `docs/sessions/2026-05-19-geometry-v9-close.md`.
- **2026-05-15 — Phase 3.1.7.5 `crd-units` CLOSED** — two-layer typed architecture; ADR-0078; `crd-no-untagged-physical-numeric` guard.
- **2026-05-10 — Phase 3.0 scene/ECS CLOSED** — 8-layer slot ECS; ADRs 0049–0061.

For complete chronology see `docs/sessions/`; for the ADR index `docs/decisions/README.md`; for the active phase plan `docs/phases/phase-3.1.6-hesap.md`.

> Older milestones intentionally truncated — they live in session logs.
