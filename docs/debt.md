# Cerid — Open Debt

Items that are not blockers but should not be forgotten. When picked up,
move to a session log entry and remove from here.

## Active debt

### `b16-b4-ocean-mesh-followons` — B16/B4 open follow-ons after the displaced-ocean + mesh-shader session (filed 2026-07-16)

> Not blockers — the ocean renders (Vulkan) + the mesh path is proven; these finish B16/B4 in gold standard. Session:
> `docs/sessions/2026-07-16-b16-displaced-ocean-mesh-shaders.md`. Mesh device scars: [[feedback_mesh_shader_device_scars]].
>
> 1. **B16-close DoD** (owed before B16 closes) — clang-tidy on the touched headers (`ckir.hpp`, `ckir_glsl.hpp`, `ckir_hlsl.hpp`,
>    `ckir_water_render.hpp`) + the gpu-context sources, and the **4-config per-slice sweep** (win-debug + clang-cl + asan +
>    shipping, both backends). This session verified only `win-release` (the ocean render is a `[.ocean-frame]` visual test).
> 2. **DX12 device mesh render** — `emit_mesh_hlsl` is DXC-validated (compiles to DXIL) but there is NO DX12 device draw yet: needs
>    a mesh-shader PSO (MS+PS) + `DispatchMesh` + a `create_mesh_program`/`draw_mesh` in the DX12 raster context (mirrors the
>    Vulkan `VK_EXT_mesh_shader` path). Also the **TASK/amplification** stage, and **B4-vis** (visibility buffer) / **B4-tess**.
> 3. **WGSL/MSL portability of the cascade sampling** — `SampleIndexedLod` is bindless (descriptor-array); WebGPU has no bindless,
>    so the portable form is a `texture_2d_array` layer + `textureSampleLevel`. Rework the ocean cascade textures to a layered
>    array so the vertex-pull ocean lowers to WGSL/MSL too (mesh shaders don't exist on WebGPU — that stays vertex-pull anyway).
> 4. **Minor ocean visual polish** — a faint residual horizon line, a slightly softer sun, more directional god-ray shafts.
>    → [[project_ocean_visual_gaps_before_b16_close]].

### ✅ `ckir-offhost-emitter-cse` — FIXED 2026-07-15 (CUDA/MSL/WGSL compute emitters now CSE like GLSL/HLSL). No remaining debt.

> **Was:** the **GLSL/HLSL** compute-kernel emitters materialize every arithmetic node as a temp keyed by node id (`temped[]`), so a
> shared subtree emits ONCE. The three off-host emitters — `emit_compute_kernel_cuda` / `_msl` / `_wgsl` — instead **inline-expanded**
> each value recursively (`ev(self, nd.a)`) with **no temp cache**, re-emitting a node's whole subtree once per reference. Shallow
> kernels (the `build_reverse` gate, FFT/transpose butterflies = one-shallow-expression-per-statement) were fine — which is why it
> was never caught — but a **DEEP shared value DAG** (the B15-b Perlin-Worley cloud density: a 3-octave Perlin FBM + the
> Burtle-Jenkins hash in one final-store expression) expanded EXPONENTIALLY: emitting it to CUDA/MSL/WGSL exhausted a 128 MB TLSF
> arena (`TlsfAllocator: out of memory`).
>
> **Fix (this session):** gave all three off-host emitters the same node-id materialization the GLSL/HLSL path uses — an
> `is_inline_op` predicate + a `decl` pass that emits every non-inline arithmetic node ONCE as a `t<node>` temp (children first),
> keyed by the existing `matd[]` array, and wired `decl(...)` before each statement's value/index in `emit_body`. Leaves + cast/
> select/compare/bitops stay inline. Determinism is unaffected (CUDA compiles `--fmad=false`; the temps only dedupe). The five-backend
> emit gate `tests/kir/test_ckir_kernel_emit.cpp` "Perlin-Worley cloud density emits on ALL backends" now passes; all existing
> off-host structural `has(...)` checks (reverse/select/FFT) were UNCHANGED (materialization only adds temp-decl lines; the checked
> substrings are signatures/barriers/inline ops). Full kir 30554/153 green. Mission "portable across ALL backends" now holds for
> deep kernels at the emit level (real CUDA/Metal/WebGPU compile+run remains ADR-0098 Part C). → this session (2026-07-15 clouds).

### `dct-gcc-f32-werror-conversion` — pre-existing gcc-f32 `-Werror=conversion` in `dct.hpp` (found 2026-06-15)

`engine/hesap-fft/include/crd/hesap/fft/dct.hpp:244` (and the sibling DCT-III/DST-III direct loops) computes
`std::sin(pi * static_cast<double>(2*nn+1) * static_cast<double>(k+1) / (2.0*n))` where `nn,k,n` are `usize`;
the **f32** instantiation (`DctPlan<float>::direct_dst3`, triggered by `test_dct.cpp`) trips gcc
`-Werror=conversion` (`unsigned long → double` may change value). This blocks the WSL-gcc build of
`crd-hesap-fft-tests`. **It is latent because the v10-f DCT slice was DoD'd only on the 4 Windows configs**
(MSVC accepts it silently) — the documented `feedback_T_double_literal` / no-`T{double_literal}` hazard class.
**Fix:** wrap the integer subexpressions in explicit `static_cast<double>` (or compute the angle from an `int`
index), then re-confirm the DCT gates stay bit-identical on Windows. Not fixed in the FFT small-N slice (off
its scope; the FFT code itself is gcc-strict-clean). → `docs/sessions/2026-06-15-fft-small-n-engine-crush.md`.

### ✅ `mf-lu-frontparallel-flaky-uaf` — FIXED 2026-06-09 (root cause: `TlsfAllocator::init_pool` end-sentinel mistiling). No remaining debt.

> **FIXED at the allocator root.** The pre-existing flaky crash (~20/30 runs) in the PARALLEL multifrontal-LU
> front-walk (`tests/hesap-direct/test_multifrontal_lu_pp.cpp` → "CONNECTED large grid", `grid3d_weak(22)`,
> nw∈{2,4,8}; always an AV in `TlsfAllocator` coalescing a free block whose links were overwritten) was a bug in
> **`TlsfAllocator::init_pool`**, the engine's most-used allocator.
>
> **Root cause (pinpointed):** `init_pool` reserves `free_block_size = capacity − 3·16` (start sentinel + free
> block's own header + end sentinel = 48 B), but placed the **end sentinel** at `base + 16 + free_block_size`
> (= `capacity − 32`) — accounting for only ONE preceding header, **16 bytes too early**. `block_next(free_block)`
> correctly lands at `base + 32 + free_block_size = capacity − 16`, so it **overshot the (mis-placed) sentinel into
> the chunk's uninitialised tail slack**, whose low bit reads as `kFreeBit`. The moment a block reached the chunk's
> end and was freed, coalescing merged that garbage → a free-list smash. **Benign until a chunk fills to its tail.**
> That is why it only ever surfaced under the **multi-chunk** GrowableTlsf path (the LU's heavy front alloc/free/
> **reuse** churn grows the pool into 256 MB chunks and fills them to the end); a single big chunk never fills to its
> tail, and the memory tests never allocated-to-tail-then-freed — so 820 K+ assertions passed over it for a long
> time. It is **placement, not a race** (hence ts-wrap serialized everything and still crashed; TSan clean).
>
> **The fix (`engine/memory/src/allocators/tlsf_allocator.cpp`):** place the end sentinel at
> `base + 2·kBlockHeaderOverhead + free_block_size` (= `block_next(free_block)` = `capacity − 16`), so the three
> 16-byte headers + payload tile the chunk exactly. One line; compiler-agnostic pointer arithmetic.
>
> **The LU single-chunk `factor_pool` workaround was REMOVED** — `factor_attempt` is back to a plain
> `ThreadSafeAllocator ts(m_alloc)` over the caller's (multi-chunk) allocator. The real v7-e-2 GEMM arena
> (`&gemm_arena` on `factor_front`) is unrelated and kept.
>
> **Verified:** clean shipped binary (no diagnostics, multi-chunk path forced) — grid3d moat **30/30 clean**;
> `crd-memory-tests` 820 014 assertions / 108 cases green with the fix relinked; win-debug `ctest -R "memory|hesap"`
> 48/48; the {1,2,4,8}-worker determinism moat intact. The previously-noted `growable-tlsf-multichunk-freelist`
> "remaining debt" **does not exist** — there was no separate multi-chunk free-list bug; the multi-chunk path was
> simply the only consumer that filled a chunk to its tail and tripped the single `init_pool` defect.
>
> **Lesson for the test suites:** the foundational allocators are validated by volume, not by boundary adversaries.
> A deliberate fill-to-tail / fragment-to-end TLSF stress test would have caught this years earlier — worth adding.

### ✅ `gemm-parallel-frame-arena-leak` — RESOLVED 2026-06-05 (v5f-c, central mark fix)

> **RESOLVED.** Added the scoped frame marker to the jobs API (`frame_get_mark()`/`frame_set_mark()` +
> `FrameArena::set_cursor()`) and used it inside `gemm_parallel` + `small_gemm_parallel`: each call saves
> the dispatching thread's frame-arena mark and restores it after every `parallel_for` `wait()`,
> reclaiming only its own JobDecl arrays in place (nest-safe — preserves a caller's frame state below the
> mark, which `frame_reset()` cannot). Verified: a 4000-call leak-regression test (`[frame-arena]`,
> ColMajor main path) that previously exhausted the 1 MB arena at ~1000 calls now completes; jobs
> (29244/90) + hesap-dense (354905/346) green; clang-cl + win-asan + win-tidy clean. The driver-level
> `frame_reset()` workarounds (supernodal_cholesky/lu, multifrontal_lu/qr/ldlt, blr `reclaim_frame_arena`)
> are KEPT — they reclaim the drivers' own *direct* `parallel_for` JobDecls (e.g. `factor_cholesky`,
> cholesky.cpp:156), which the gemm-scoped fix does not cover (advisor-scoped: a `parallel_for`-level
> self-clean is a core-primitive contract change for a separate slice). Original entry below for history.

### `gemm-parallel-frame-arena-leak` — filed 2026-06-04 (v5e-3 Leg B)

> **`crd::hesap::dense::gemm_parallel` (`blas3.cpp`) `frame_alloc`s its per-call JobDecl arrays
> from the per-thread jobs FrameArena but NEVER reclaims them** — the in-code comment
> ("No frame_reset here — it would invalidate frame_alloc state the CALLER may hold") documents
> the deliberate non-reset. Each call leaks ~24 KB; a caller that fires many gemms without an
> external `frame_reset()` monotonically fills the 1 MB arena and **exhausts** it. Surfaced in the
> v5e-3 node-parallel multifrontal Cholesky: a single big front's factor fires hundreds of
> `gemm_parallel_auto` calls ⇒ exhaust mid-front (win-asan: `frame_arena.hpp:60` assert; linux-release:
> assert compiled out ⇒ pointer past the arena ⇒ glibc `corrupted size vs prev_size`).
>
> **Localized workaround SHIPPED in v5e-3 (blr.cpp `reclaim_frame_arena()`):** the driver calls
> `jobs::frame_reset()` after each parallel gemm in the factor hot loops (safe: serial dispatch from
> the main thread, each gemm `wait()`s before returning ⇒ valid frame boundary, no caller holds state).
>
> **Proper central fix (this debt):** add a per-thread **scoped frame marker** to the jobs API
> (`frame_get_mark()`/`frame_set_mark()` — save offset on entry, restore on exit) and use it inside
> `gemm_parallel` so each call reclaims ONLY its own allocations, correct even when nested under a
> caller that holds frame state (which the current all-threads `frame_reset()` cannot be). Removes the
> need for callers to know about the leak. Blast radius = every gemm caller ⇒ needs its own verified
> change (5-config + the parallel benches), not a mid-crush edit. Until then the localized reclamation
> is the safe path for new parallel direct-solver code.

### ✅ `v1a-3-assembly-smalln` — RESOLVED 2026-05-20 (same day as filing)

> Sparse COO→CSR/CSC assembly now **beats Eigen `setFromTriplets` at every size** (win-release, i9-class, best-of-3, f64): N=50k **1.03×**, N=200k **1.44×**, N=1M **1.76×** (was 0.74× / 1.16× / 1.19×). The candidate fix landed: `assemble<ByRow>` scatters **directly** into the final `inner_idx`/`values` (no `Entry` AoS), uses in-place parallel-array insertion sort for small inner vectors and a single reused merge-sort scratch for large ones (dense-row robustness preserved), and dedup-compacts in place. The last increment was a new `crd::containers::Array::resize_uninitialized` (trivially-constructible T only; value-inits otherwise) used for the two fully-scattered arrays — eliminating the zero-init pass that dominated the small-N residual. Verified by `bench_hesap_sparse_assembly_vs_reference`.

### Future cluster — direct-manipulation UX (gizmos / mesh + curve + navmesh editors) — filed 2026-05-19

> **Not a follow-on; a future workstream cluster.** User flagged (during
> Phase 3.1.7 v10e control-point editing question) that direct-
> manipulation UX is a high-priority future area requiring its own
> session cluster. Slated for the UI phase, possibly preceded by a low-
> level plumbing detour. Until then, sandbox showcase scenes use ImGui
> DragFloat3 / sliders for control-point + parameter editing.
>
> **Scope (user's framing):**
>
> - **Transform gizmos** — translate / rotate / scale handles on selected
>   entities, screen-space picker + camera-relative axis lock.
> - **Curve control-point gizmos** — drag curve control points in the 3D
>   viewport. v10e's ImGui DragFloat3 panel is the temporary surface
>   the gizmo cluster eventually replaces.
> - **NavMesh editing** — vertex / edge / face manipulation.
> - **Mesh editing — Blender-class** — vertex / edge / face select +
>   deselect + drag (equal-class consumer alongside game + cinematic).
> - **Selection state primitives** — multi-select, select / deselect,
>   marquee, hover-highlight.
>
> **Likely module surface (placeholder, not committed):** `crd-ui-gizmo`
> (ray-vs-handle hit-testing + drag-state machine + axis-locked
> translation) consumed by sandbox + future editor.
>
> **Architecture written up 2026-05-22 → `docs/phases/phase-ui-tooling.md`**
> ("Cerid UI & Tooling Architecture — `crd-ui` · gizmos · editor overlays").
> Elite design captured: **two worlds** (document vs transient tooling world —
> gizmos/editor-UI are `EditorOnly`-tagged entities, hidden from the scene-tree
> panel, not saved, not in the user's undo stack); the **Logic / Visual /
> Command triple** (gizmo = a System + a swappable Visual [entities OR
> `crd-geometry-viz` immediate-draw] + a committed command-verb); gizmos and UI
> are the same pattern (entities + systems + command-verbs); lifecycle
> (selection→spawn→drag→commit-one-command-on-release); hit-test reuses
> `crd-geometry-spatial`; elite traps (constant screen-size, overlay-pass depth,
> pointer-capture, selection-as-shared-state, gizmo-never-the-only-path). UI
> rendering = screen-space 2D frame-graph pass vs worldspace renderable;
> shader/resource changes ADDITIVE not structural; `crd-font` (MSDF) is the real
> new substrate. Sequences after renderer + `crd-font` + `crd-scene` + the
> command layer (`docs/phases/phase-4.0-platform.md`).
>
> **Sequencing:** undecided. Slots in EITHER after `crd-hesap-dense` v0
> + Phase 3.1 eylem v1c-resume (consumer-pull from eylem's
> collider/joint editors) OR before, depending on when editor UX
> becomes the critical path. Discussed at the close of Phase 3.1.7.
>
> **Until then:** all geometry + curve + future mesh-viz sandbox scenes
> use ImGui DragFloat3 / sliders — do NOT bake a half-built ad-hoc
> picker into individual scenes.

### `v2b-amd-cs_amd-tiebreak-isolate` — AMD fill on bcsstk25 (1.044× Eigen) — filed 2026-05-21

> **User-sanctioned optimization follow-on, NOT a defect or defer.** v2b AMD
> (`amd_order`) lands at 0.989× / 1.002× / **1.044×** Eigen-AMD `nnz(L)` on
> bcsstk13/24/25 — beats, ties, and 4.4%-above respectively. The ≤1.05× gate is
> met on all three. The bcsstk25 residual is an **un-isolated tie-break /
> iteration-order divergence from CSparse `cs_amd`** (all algorithmic features —
> approximate degree, mass elimination, supervariables, aggressive absorption,
> dense-node-last — are faithfully ported; the degree formula is algebraically
> identical).
>
> **Why it's not a defer (user-confirmed 2026-05-21):** AMD is a *heuristic* —
> faithful implementations (SuiteSparse `amd_2`, `cs_amd`, MATLAB, METIS-AMD)
> differ a few % matrix-to-matrix; "Eigen's exact number" is one impl's output,
> not a floor. Fill is a **downstream-perf knob (factor memory + flops), never
> correctness** — any permutation yields the identical solution. Across a corpus
> we're at parity-or-better (we beat Eigen on bcsstk13).
>
> **If revisited:** isolate the divergence (likely cs_amd's incidental node-scan /
> supervariable-principal order). Matching it bit-for-bit would re-pin D(ord)-5
> off "lowest-index principal" — a determinism-cleanliness trade for a few % on
> one matrix. **Real trigger:** an end-to-end v5 sparse-solve benchmark showing
> ordering fill (not the numerical kernels) is the bottleneck on a real workload.

### `v2e-weighted-compression` — ND fill loses Eigen-AMD on bcsstk25 (multi-DOF) — filed 2026-05-21 — ✅ RESOLVED 2026-05-28 (premise FALSIFIED by benchmark)

> **✅ RESOLVED 2026-05-28 — premise falsified by measurement; no code shipped (v5a-0).**
> Implemented supervariable graph compression (identical-closed-neighbourhood merge)
> with vertex-weight propagation through the ND bisection + CAMD `nv`, exactly as this
> entry prescribed, then benchmarked `bench_hesap_ordering_vs_reference` (Eigen-AMD peer).
> **Result: compression REGRESSED ND fill on all three matrices** (uniform ~5–12% worse):
> | matrix | compress engaged | un-compressed ND | compressed ND |
> |---|---|---|---|
> | bcsstk13 | 20.5% reduced, maxw=6 | **254079 (0.984× WIN)** | 273162 (1.058× lose) |
> | bcsstk24 | 75.0% reduced, maxw=6 | 285920 (1.001× tie) | 320113 (1.121× lose) |
> | bcsstk25 | 14.6% reduced, maxw=3 | 1670446 (1.157× lose) | 1714528 (1.187× lose) |
> The un-compressed numbers reproduce this entry's original baseline exactly.
> **Structural reason:** CAMD already detects supervariables DURING elimination (the
> `nv`-weighted approximate-degree update + the in-loop indistinguishable merge), at the
> granularity that helps. Pre-compression delivers that information earlier but COARSER —
> it strictly reduces the bisector/CAMD's choices, so it cannot beat un-compressed on a
> graph where un-compressed already wins (bcsstk13). The "DOFs split across separator
> classes" hypothesis is wrong: the un-compressed ND already handles them. **Reverted in
> full** (compression kernel + weighted plumbing); the kernel + this measurement are the
> durable artifact. **Status: closed.** ND legitimately loses to AMD at bcsstk13/24/25
> sizes (the win-small-lose-large inversion this entry flagged); **our AMD already beats
> Eigen-AMD** (1.039 / 0.960 / 1.007 — GATE-OK), so the v5 sparse-direct consumer picks
> AMD on these matrices. Large-3D-elliptic where ND wins asymptotically is HSS-front
> territory (v5e), not a fill-ordering question. Vindicates the benchmarks-at-slice-close
> mandate: unit tests (correctness) passed; the benchmark caught the net regression.
>
> --- original entry (premise, now falsified) ---
>
> **Tracked optimization follow-on, NOT a defect.** v2e nested dissection + CAMD
> **beats Eigen-AMD fill on bcsstk13 (0.983×) and bcsstk24 (0.999×)** but loses on
> **bcsstk25** (1.158× vs Eigen-AMD; n=15439, a tall 3D skyscraper stiffness matrix
> with multiple DOFs per node) — the *opposite* of the textbook ND-asymptotic
> pattern (win small, lose large). Fill is a **downstream-perf knob (factor memory +
> flops), never correctness** — `nd_order` always yields a valid permutation, so
> every solve is identical; and the v5 consumer can pick the better of AMD/ND
> per-matrix anyway (both are available).
>
> **Root cause:** bcsstk* are structural matrices with groups of identical-pattern
> rows (the multiple DOFs of one mesh node). AMD merges these via supervariables;
> our CAMD gates supervariable merge by `cmember`, so when a separator splits a
> node's DOFs across classes they can't merge → fill penalty, worst on the
> largest/densest case (bcsstk25). Verified via the path test: 1D is ND's worst
> case (AMD provably optimal) and CAMD-uniform reproduces AMD exactly, so the port
> is correct — the gap is purely separator/multi-DOF quality.
>
> **The fix (CHOLMOD/METIS technique):** supervariable **graph compression** —
> merge indistinguishable (identical-closed-neighbourhood) vertices into one
> super-vertex *before* ND, run bisection + cmember + CAMD on the compressed graph,
> expand after. A first cut was implemented; **unweighted** compression *regressed*
> all three matrices (it imbalances the bisection — supers counted as weight-1) and
> was reverted. The real fix needs **vertex-weight propagation**: thread member
> counts as `vwgt` through `assign_cmember` → `bipartition_refined` → `to_weighted`
> and into CAMD's initial `nv`, so the compressed bisection balances by original
> count and CAMD's degree accounting is correct. ~150 LOC, uncertain but likely
> flips bcsstk25. **Real trigger:** a v5 sparse-direct benchmark showing ND-fill
> (not the numeric kernels) is the bottleneck on a multi-DOF FEM workload.

### `v3b-1b-perf-followon-qr-block_reflector-consolidate` — unify qr.cpp + orgbr WY helpers — filed 2026-05-23

> **Mechanical dedup, NOT a defect.** v3b-1b-perf added shared
> `detail/block_reflector.hpp::build_block_t_from_vtv` (the `dlarft` factor) and
> uses it from `detail/orgbr.hpp`. `qr.cpp` still carries its own local copies of
> `build_block_t_from_vtv` + `materialize_panel_v` (anonymous-namespace, identical
> logic). Migrate qr.cpp to the shared header to remove the duplication — the same
> promote-then-consolidate pattern already filed for `dot_simd`
> (`v3b-1a-perf-followon-dot_simd-consolidate-eig_sym`). Low risk (relocate
> identical code; qr ctests gate it); deferred to keep the v3b-1b-perf slice's
> blast radius tight while the v3a-3/v3b tree is uncommitted.

### `v3b-1c-blocked-complex-bidiag` — blocked complex zgebrd for the at-scale complex SVD crush — filed 2026-05-23

> **Perf follow-on (correctness done), NOT a defect.** v3b-1c complex SVD is correct
> + gated (A=U S V^H <1e-9) and reuses the crushing real D&C/dbdsqr for the
> bidiagonal-SVD part, so it's competitive at moderate N. But the complex
> *bidiagonalization* (`bidiagonalize_complex`, zgebd2-class) is UNBLOCKED — at
> scale that's the bottleneck (exactly like the real path before v3b-1a-perf's
> blocked dlabrd). The at-scale complex speed-crush vs Eigen complex SVD + LAPACK
> zgesvd/zgesdd needs a **blocked complex `zgebrd`** (complex `zlabrd` panels +
> BLAS-3 trailing update via complex `gemm_parallel_auto`) + a complex `dorgbr`
> (zungbr) for the form-Q/P. Mirrors the real v3b-1a-perf/dorgbr work. Add a
> 4-column complex bench (Eigen BDCSVD-complex + LAPACK zgesvd/zgesdd) when this lands.

### `v3b-1c-svdvals-dqds-direct` — values-only complex svdvals via dqds — filed 2026-05-23

> **Perf follow-on, NOT a defect.** Complex `svdvals` currently routes through the
> full complex `svd` driver (computes singular vectors then returns the spectrum) —
> ~2× wasteful. The values-only path: complex bidiag → real (d,e) → dqds (`dlasq2`)
> directly, no vectors (mirrors the real `svdvals`). Factor the real svdvals'
> dqds-on-(d,e) extraction into a shared helper + call from the complex path.

### `v3b-3-nystrom-cholesky` — PSD-streaming Nyström variant of rsyev — filed 2026-05-23

> **Optional alternative, NOT a defect.** v3b-3 `rsyev` uses Rayleigh-Ritz
> (`QᵀAQ` + `eig_sym`) — general (any symmetric A) + reuses the eig_sym we beat
> Eigen+LAPACK with (D(svd)-15). The Tropp-2017 Nyström `C⁻ᵀ` form (`Y=AΩ`,
> shift ν, `B=ΩᵀY_ν` SPD, Cholesky `B=CCᵀ`, `F=Y_ν C⁻ᵀ`, svd(F)) is more
> accurate for PSD streaming/fixed-rank approximation. Add it as a `rsyev_psd`
> specialization if a PSD-streaming consumer arrives. Reuses Cholesky + trsm
> (both shipped v0e). Low priority — Rayleigh-Ritz covers the gate.

### `v3b-2-parallel-merges` — parallelize dlasd0's same-level merges — filed 2026-05-23

> **Optimization follow-on (we already win), NOT a defect.** v3b-2 D&C SVD already
> beats Eigen `BDCSVD` 1.59–3.21× / LAPACK `dgesdd` 1.37–4.55× at N=128–1024 via
> the parallel BLAS-3 back-transform + dlasd3 merge assembly (`gemm_parallel_auto`),
> with `dlasd0`'s recursion itself SERIAL. The independent same-level merge nodes
> (`dlasd1` calls within one `DO I=LF,LL` level) can run concurrently across
> `crd::jobs` — the lever that would widen the lead further (cores LAPACK/Eigen
> serial D&C lack at the merge level too). Per-merge Tlsf arena + bit-exact across
> worker counts. Deferred: the gate ("beat Eigen+LAPACK at scale") is already met;
> this is pure margin. Watch the frame-arena hazard ([[feedback_jobs_parallel_for_frame_arena_exhaustion]]).

### `v3b-2-svd-via-mrrr` — novel D&C-alternative SVD fork — filed 2026-05-23

> **Deferred research fork, NOT scope reduction.** The v3b-2 locked design flagged
> two routes to the full-SVD-at-scale crush: (1) Gu-Eisenstat D&C `dbdsdc`
> (chosen — the references' own algorithm, mirrors the winning Cuppen eigensolver),
> and (2) **SVD-via-MRRR** — form `J=[[0 Bᵀ][B 0]]` (2n×2n symmetric tridiagonal,
> ±σ eigenvalue pairs) and run the parallel MRRR (which already crushes LAPACK on
> the eigenvector path). The blocker: J's eigenvalues are EXACT ±σ multiples (every
> σ appears twice) → MRRR's cluster loop can't separate exact multiples → Gram-
> Schmidt fallback for every value defeats the O(n²) win. It needs a bespoke
> perfect-shuffle extraction (`[u;±v]/√2`) — a new driver, genuinely novel, real
> rabbit-hole risk. **Pursue only if Gu-Eisenstat D&C (v3b-2) does not reach the
> crush** (unlikely — Cuppen D&C already beats both Eigen + LAPACK).

### `v2c-small-n-analyze-constant-factor` — symbolic analysis 0.80× Eigen at n=2003 — filed 2026-05-21

> **Tracked perf follow-on, NOT a defect or defer.** v2c `symbolic_factorize` and
> its `nnz_l` analysis path beat Eigen `analyzePattern` at n=3562 (1.77×) and
> n=15439 (1.49×), but trail at the smallest test matrix bcsstk13 (n=2003) at
> **0.80×** (ours 1.65 ms vs Eigen 1.32 ms, Δ≈330 µs). The **pattern gate (the
> contract) passed bit-exact on all three** — this is purely the symbolic-analysis
> *timing* on one small matrix.
>
> **Why it's a constant-factor, not algorithmic (advisor-confirmed 2026-05-21):**
> our symbolic scales *better* than Eigen's — across the 7.7× n-range (2003→15439)
> our time grows 2.4× while Eigen's grows 4.4×. The crossover is between n=2003 and
> n=3562. That signature is fixed overhead, almost certainly `build_adjacency`
> (allocate + symmetrise `A∪Aᵀ` + per-row ascending sort) which is amortised away
> by n=3562. cs_counts itself is O(nnz(A)·α) — the asymptotically right choice
> (cheaper than a counting `cs_ereach` pass for high-fill matrices).
>
> **If revisited:** profile `build_adjacency` at small n; candidate wins —
> skip the re-sort when the input CSR is already sorted + symmetric (the SPD
> common case), or fuse the symmetrise into the etree pass. Low priority: 330 µs
> at the smallest problem size, on a step that is already faster than Eigen at
> every n that matters for sparse direct. **Real trigger:** a workload dominated
> by repeated small-matrix symbolic analysis (e.g. many-small-system batching).

### ✅ Phase 3.1.7 v9a-b1 follow-on — AVX2 vectorised CPU radix sort — CLOSED 2026-05-18 (same day as filing)

> **STATUS — CLOSED 2026-05-18.** Investigated three SOTA radix techniques
> via web research (Wassenberg 2010, Satish 2010 Intel paper, RADULS Kokot
> 2017) + measured median-of-5 on win-shipping with `/O2 /LTCG /OPT:ICF
> /arch:AVX2`. All three were SLOWER than scalar at the 1 M u32 / 8 MB
> working set:
>
> - AVX2 8-wide SoA sub-histograms: 0.45x (slower; AVX2 has no vector scatter)
> - Wassenberg SWWC + AVX2 burst flush: 0.63x (slower; L2-resident working
>   set doesn't trigger the RAM-bound regime SWWC attacks)
> - Multi-pass histogram fusion: marginal benefit offset by extra allocation
>
> **Winning technique = scatter-side prefetch.** Single `_mm_prefetch` hint
> 8 iterations ahead of the actual store: **5.32 ms → 4.99 ms = 1.07x on
> 1 M u32 win-shipping median-of-5**. Determinism intact (prefetch is a
> hint, never writes state); output byte-identical to v9a-b1 reference
> (40 134 assertions across 20-case adversarial corpus preserved).
>
> Final perf headroom on 1 M u32: 4.99 ms measured vs **20 ms asserted
> NDEBUG budget = 4.0x**.
>
> Pinned design decisions D145 (scalar+prefetch is elite at this scale),
> D150 (prefetch distance = 8 iters covers ~40-cycle L1 miss), D151
> (parallel-radix + AVX-512 vpscatterdd filed for a future consumer that
> hits the wall) for ADR-0076 §25 amendment at v9a-close.
>
> Session log + sources + full negative-finding write-up:
> `docs/sessions/2026-05-18-geometry-v9a-b1-simd-close.md`.
>
> **Future >5 ms paths if a consumer ever surfaces the need:**
> - Parallel radix via `crd-jobs::parallel_for` (bandwidth-bound ⇒ 2-3x
>   from cores, with deterministic per-worker stable-merge).
> - AVX-512 `vpscatterdd` true vector scatter (not in current CI matrix).

### ✅ Phase 3.1.7 v9a-a — four follow-ons PAID 2026-05-18 (same day as filing + reversal)

> **STATUS — CLOSED 2026-05-18.** All 4 follow-on slices shipped same day:
> `v9a-a-typed` ✅ + `v9a-60bit-cpu` ✅ + `v9a-a-async-compute` ✅ +
> `v9a-60bit-gpu` ✅. **5-config DoD PASS** (`scripts/per-slice-check.ps1
> -IncludeRelease -Parallel`, 39 s). Consolidated record in
> `docs/sessions/2026-05-18-geometry-v9a-a-followons.md`. Pinned design
> decisions D137-D140 carried for ADR-0076 §25 amendment at v9a-close.
>
> Cluster-level effects of the paydown:
> - RHI surface: two new virtuals on `crd::rhi::Device` (`create_command_buffer_for_queue` + `supports_shader_int64`), both appended at END per D135 vtable-stability. Vulkan backend implements both; smoke + test mocks updated.
> - rhi-vulkan now lazy-creates a compute-family `VkCommandPool` when the device has a dedicated compute family + enables `shaderInt64` feature when supported.
> - `crd-geometry-bvh-gpu` ships CPU+GPU 30-bit (raw + typed) AND CPU+GPU 60-bit Morton paths, plus a sync+async dispatch surface.
> - v9a-b1 sort API can now template + INSTANTIATE over both `KeyT=u32` (30-bit) and `KeyT=u64` (60-bit) from day 1 — both backends are real shipping code, not a hypothetical hook.
>
> Per the refined [[ship-at-consumer-template-from-day-one]] rule: this
> was the correct call for SUBSTRATE work where the engine IS the
> product. The original "defer until consumer" rule was applied wrongly
> at the first v9a-a close; user pushback got us to the elite answer.

---

### Shewchuk adaptive predicates — FULLY PAID 2026-05-17 (orient3d + incircle Stage D 2026-05-14; insphere Stage D 2026-05-17 v8c-pre)

> **STATUS — CLOSED 2026-05-17 by Phase 3.1.7 v8c-pre.** All three adaptive
> predicates (`orient3d_exact`, `incircle_exact`, `insphere_exact`) ship full
> Shewchuk Stage D. Adversarial cospherical corpus (`[stage-d][adversarial]`
> tag in `tests/geometry-primitives/test_predicates.cpp`) verified — the
> previous Stage-A-equivalent `insphere_exact` returned `-16777216` on the
> r²=5×10⁹ non-symmetric cospherical configuration; the Stage D port returns
> exact zero. No outstanding adaptive-predicate debt.

**Historical entry follows for context:**

### Shewchuk adaptive predicates — partial paydown 2026-05-14 (orient3d + incircle full Stage D shipped; insphere remains Stage A-equivalent)

> **TL;DR — WHEN TO PAY THIS DEBT:**
>
> 1. **PRIMARY TRIGGER (mandatory paydown):** **At the start of Phase 3.1.7 v8a (2D Bowyer-Watson Delaunay)** — re-verify `incircle` Stage D against Bowyer-Watson's stress corpus (cocircular point clouds; the canonical case where any robust 2D Delaunay impl is judged). If failure surfaces, it's a Stage D bug in `incircle_exact` — fix BEFORE shipping v8a.
> 2. **PRIMARY TRIGGER for insphere (mandatory paydown):** **At the start of Phase 3.1.7 v8c (3D Bowyer-Watson Delaunay)** — `insphere_exact` MUST be upgraded from the current Stage-A-equivalent re-expression to full Shewchuk expansion arithmetic BEFORE v8c writes any tetrahedron-flip logic. Upgrading at v8c-time means the implementation lands with a real workload (Bowyer-Watson's cospherical adversarial corpus) to validate against — eliminates the silent-correctness-debt risk the advisor flagged. v8c session log must explicitly verify this paydown.
> 3. **EARLY TRIGGER:** Any consumer (V-HACD v9c, CFD AMR Phase 3.1.10, FEA contact Phase 3.1.12, CAD boolean Phase 3.1.8) reporting wrong-sign behavior on cospherical-class input → promote this entry to "blocker" + fix immediately. The current `insphere_exact` will silently mis-sign true cospherical input, so the trigger isn't "tests fail" — it's "downstream geometry surfaces wrong topology" (flipped tetrahedra in a mesh / wrong inside-outside on a thin shell / etc.).
> 4. **DO NOT SPECULATIVELY PAY:** Shipping ~2000 LOC of intricate expansion arithmetic without a workload to validate against is silent-correctness debt of its own — see the "Why insphere stopped" rationale below.

**Paid 2026-05-14:**
- **`orient3d`** ✅ — Full Stage D shipped (`orient3d_exact` in `predicates.cpp` lines ~610-680). Six 4-element 2x2-minor expansions + four 24-element triangle-cofactor expansions + cascaded sums to a 96-element final expansion. Returns the exact sign of the highest-magnitude nonzero term. Validated with the v3a-debt adversarial corpus: 4-truly-coplanar-points-on-slanted-plane returns exact zero; near-coplanar tiny-perpendicular-perturbation at scale 100 returns correct sign.
- **`incircle`** ✅ — Full Stage D shipped (`incircle_exact`). New helper `expansion_product` does general N×M expansion-by-expansion multiplication (used to multiply the 4-element x²+y² lift expansions by the 4-element xy-minors). Each cofactor is a 96-element expansion; final det is up to 384 elements. Validated: 4 cocircular points on radius-1e3 circle return exact zero; tiny-perturbation-outside returns negative. **Re-verification scheduled at Phase 3.1.7 v8a Bowyer-Watson 2D Delaunay start** — the consumer-validation step that confirms the Stage D port is correct on a real workload.

**Still deferred:**
- **`insphere`** ⚠️ — The 5x5-cofactor Laplacian STRUCTURE is in place (`insphere_exact` decomposes into 5 `det4_3d` sub-determinants per the published Shewchuk pattern), BUT the inner products in each `det4_3d` use f64 multiplication directly (not expansion arithmetic). This means `insphere_exact` is **algorithmically equivalent to a clean re-expression of Stage A**, not a full Stage D in the expansion-arithmetic sense. On cospherical input where Stage A misses the sign, `insphere_exact` will also miss it.

**Why insphere stopped at Stage-A-equivalent:**
1. Full Shewchuk `insphereexact` is ~2000 LOC of cascading expansion sums (each of the 5 sub-determinants needs full 3D lift expansion through 6 minors + 4 cofactors with mixed-precision products). The complexity is one tier above incircle.
2. The advisor flagged the validation-gap problem: there is no consumer surfacing cospherical pathology to validate against. v8 Bowyer-Watson 3D Delaunay is months out.
3. Shipping ~2000 LOC of intricate expansion arithmetic without a workload to verify against is silent-correctness debt — the code can compile, pass simple tests, and harbor sign-flip bugs invisible until the v8 consumer exercises adversarial input.

The current `insphere_exact` IS a structural improvement over the previous Stage-A-only fallthrough: it reuses the 5-cofactor Laplacian decomposition that future Stage D will need, and the API surface is stable. When v8 lands, upgrading the inner products to expansion arithmetic is a localized change.

**API surface remains stable** — adaptive form drops in without API change when v8c (3D Delaunay) surfaces the requirement.

**Paydown plan (when v8c starts):**
- **Slot:** v8c-pre — the first sub-slice of Phase 3.1.7 v8c. Mandatory.
- **Scope:** upgrade `det3_lift` and the outer 4x4 cofactor sums in `insphere_exact` (`predicates.cpp`) from f64 multiplication to `expansion_product`-based expansion arithmetic. Analogous to incircle's pattern, scaled up to 3D (6-element lifts via 3 squares + 2 sums; ~3000-element worst-case final expansion).
- **Estimate:** ~800–1200 LOC engine + ~300 LOC tests + ~4–5 days calendar.
- **Validation:** v8c Bowyer-Watson 3D Delaunay's cospherical-input stress corpus is the workload — port should produce flip-free tetrahedra on Shewchuk's canonical adversarial input.
- **Session log entry required:** v8c's session log must explicitly mark this paydown complete, with a link back to this debt entry. Don't let the debt slide silently into v8c — make the paydown a NAMED v8c-pre slice.

**Reference implementation:** Shewchuk's `predicates.c` v4.0.0 from https://www.cs.cmu.edu/~quake/robust.html. The `insphereexact` function (lines ~3500-3800 of the v4.0.0 source) is the literal port target. The general `expansion_product` helper already shipped in 2026-05-14 paydown makes the inner-product upgrade mechanical rather than algorithmic — most of the v8c-pre slice is structural translation, not new algorithm design.

### Transient MSVC LTCG internal compiler error on `win-shipping` `crd-sandbox.exe` link (observed 2026-05-13 v1-debts-paid sweep)

`scripts/full-sweep.ps1` returned 16/17 PASS during the v1-debts-paid verification sweep — only `win-shipping` failed, and only at the `crd-sandbox.exe` LTCG codegen phase with a fatal MSVC C1001 internal compiler error:

```
LINK : ... failed (exit code 0) with the following output:
Kod üretiliyor
D:\Dev\cerid\engine\config\src\config.cpp(245) : fatal error C1001: İç derleyici hatası.
(derleyici dosyası 'D:\a\_work\1\s\src\vctools\Compiler\Utc\src\p2\main.cpp', satır 263)
... link!DllGetObjHandler()+0x1fe99 ... CloseTypeServerPDB()+0x19fe ...
Access violation
ninja: build stopped: subcommand failed.
```

**The C1001 is not in our code.** `engine/config/src/config.cpp(245)` is plain `Config::load_from_file()` — pre-existing, unchanged in this slice. The line pointer is just where MSVC's whole-program optimizer happened to access-violate. Standalone retry of `cmake --build --preset win-shipping --target crd-sandbox` linked clean immediately with **no source change**. `win-clang-cl-shipping` (same shipping flags, different compiler) was green on the same sweep.

**Verdict:** non-deterministic MSVC LTCG internals bug, likely sensitive to the cross-TU template instantiation graph after v1 cluster's additions (new `BvhViewerCache`, `closest_point(Cylinder3)` + `closest_point(Tetrahedron)`, `tests/sandbox/test_showcase.cpp` adds new TU consumers of the same shared headers). One incident is upstream noise — the slice closes per `feedback_transient_msvc_ltcg_ice_accept.md`.

**Trigger to investigate:** recurrence on a subsequent sweep, or the same C1001 pointing at the same line range, would promote this from "transient" to "actionable upstream-workaround". Workaround candidates if it recurs:
1. `CRD_NOINLINE` on a suspected hot function in `config.cpp` or in the v1-cluster headers that LTO'd into sandbox.
2. `#pragma optimize("", off)` around `Config::load_from_file` (or the actual culprit if found).
3. Split a TU to reduce the LTCG working set.
4. Update MSVC toolchain (the canonical fix path but user-action).

**Recurrences (trigger met — now in "watch, workaround if it gets frequent" territory, still non-blocking per policy):**
- **2026-05-21 (hesap-sparse v1e-2 close):** same C1001 / Access violation in `link!DllGetObjHandler()`, this time on the `tests\sandbox\crd-sandbox-showcase-tests.exe` LTCG **link** (not config.cpp codegen) under `win-release`. 4/5 per-slice configs PASS; standalone `cmake --build --preset win-release` retry linked the same exe clean with no source change, then `ctest --preset win-release` = 2900/2900. So it remains the same non-deterministic LTCG-internals AV in the link phase, not our code. Pattern holds: it lands on whichever sandbox/showcase exe has the largest LTCG link working set. If it recurs again on the *next* sweep, apply workaround candidate 3 (split the showcase test TU) or 4 (toolchain update).
- **2026-05-21 (hesap-sparse v1f close), SAME exe again:** identical C1001 / `DllGetObjHandler` AV linking `crd-sandbox-showcase-tests.exe` under `win-release`; retry linked clean, `ctest --preset win-release` = 2909/2909. **Two consecutive win-release DoDs now ICE on the same exe → no longer "random noise"; it is reproducibly the largest-LTCG-link target.** ESCALATION: at the v1-close 18-config full sweep, if `crd-sandbox-showcase-tests` (or `crd-sandbox`) ICEs again, **apply workaround 3 (split `tests/sandbox/test_showcase.cpp` into 2–3 TUs to shrink the LTCG link set)** rather than just retrying — the retry tax is now predictable and will recur every sweep. Still non-blocking for the per-slice DoD (retry-PASS), but the workaround is now warranted, not deferred-by-default.

**Where referenced:**
- `docs/sessions/2026-05-13-v1-debts-paid.md` — the sweep transcript, decision to close on retry-success.
- `build/sweep-2026-05-13-v1-debts.log` (UTF-16) — original sweep log with the ICE.



### ~~`find_overlapping_pairs(DynamicBvh)` allocates work-stack `Array`s per call~~ — PAID 2026-05-13 (v1i-c debt-payment pass)

Added scratch-taking overload `DynamicBvh::find_overlapping_pairs(Fn&&, DynamicBvhPairScratch&)` with a `DynamicBvhPairScratch{Array<u32> walk; Array<DynamicBvhPairWork> cross}` POD constructed once from the consumer's allocator and reused across calls (`clear()` happens inside; capacity grows monotonically to the high-water mark). Existing alloc-per-call overload stays for non-hot-path callers. Facade overload mirrored. Test verifies the scratch path produces identical results to the alloc-per-call path across 5 reuse iterations with tree mutations between. Eylem v1c can now wrap the scratch overload on its broadphase hot path without per-tick alloc churn.

### ~~`Vec4f` inflate-and-slab kernel for `bvh4_shapecast_*`~~ — PAID 2026-05-13 (v1i-c debt-payment pass)

Added `ray_vs_4_aabb_inflated(...)` in `bvh4_simd.hpp/cpp` — splats `pad_{x,y,z}` to all 4 lanes, inflates each lane's bounds (`bmin − pad` / `bmax + pad`), then forwards to the existing `ray_vs_4_aabb` slab kernel. `bvh4_inflated_raycast` (the `bvh4_shapecast_*` traversal) now transposes ≤4 child bounds into SoA `Vec4f` columns and does one kernel call per node instead of four sequential scalar `inflate` + `intersect_ray_aabb_robust` calls. 2000-trial lane-by-lane test (varying pad — zero / small / moderate) confirms bit-identical results to the scalar form for finite/well-formed inputs. Mirrors v1g pattern.

### ~~Advisor #3 polish on `test_validation.cpp` BVH shift-invariance~~ — PAID 2026-05-13 (v1i-c debt-payment pass)

`BVH raycast at +1e6 origin` test now asserts AABB equivalence (named hit box at far origin shifts back to match named hit box at origin within local ULP tolerance) instead of comparing prim payload indices. A future SAH-tiebreak refactor that swaps the chosen prim on a tied hit no longer surfaces as a false test failure.


### ~~`linux-gcc-release` ctest intermittent flake~~ — ROOT-CAUSED + FIXED 2026-05-12 (jobs hardening)

Was: `linux-gcc-release` intermittently failed ctest with an opaque
`Errors while running CTest` (exit 8); a retry passed. The opaque exit
masked a crash in the test `jobs: run_and_wait from inside a worker
fiber`. Diagnosed on a native Linux VM (gdb on core dumps / live hangs);
**four distinct, all pre-existing, bugs in the fiber job system** were
behind it, fixed together:

1. **Optimizer caching the per-thread TLS base across `fiber_switch`**
   (`worker_pool.cpp`). `job_fiber_trampoline` reads thread-locals after
   the job call; a job that `jobs::wait()`s from inside its fiber can
   resume on a *different* OS thread, but the C++ abstract machine has no
   notion of that, so GCC `-O3` hoisted `lea tl_sched_ctx@tpoff(%fs:0)`
   out of the trampoline loop into a callee-saved register and reused it
   across the job call → on a cross-thread resume the trailing
   `fiber_switch` jumped onto the *wrong* thread's scheduler stack. (MSVC
   was masked by the pre-existing `/Od` on this TU.) **Fix:**
   `CRD_JOBS_TLS_OPAQUE` (`__attribute__((noipa))` on GCC, `CRD_NOINLINE`
   elsewhere) on the `tl_scheduler_context` / `tl_current_fiber_ref` /
   `tl_worker_pool` accessors, and the trampoline reaches every
   post-resume thread-local through them — forcing a fresh TLS-base load.
   This is the textbook fix for the issue (LLVM #98479 / #47179 / #63022,
   LDC #666: "define TLS accessors in a separate TU and call them").

2. **`run_job_in_fiber` left `tl_fiber` set to the parked fiber** when a
   fiber suspended (`worker_pool.cpp`) — the completed path nulled it (via
   the trampoline) but the suspended path didn't. So after a `pump()`-
   driven job suspended inside `jobs::wait()`'s spin, the *main thread's*
   `tl_fiber` was stale, and the next `jobs::wait()` on the main thread
   read it, took the fiber-suspend path with a garbage "current fiber" →
   `counter_wait` corrupted the runtime / asserted `fiber must be Active`.
   (Caught via a core dump: the asserting thread's backtrace was
   `main → jobs::wait → counter_wait`, impossible if `tl_fiber` were
   correct.) **Fix:** `run_job_in_fiber` clears `tl_fiber` on the
   suspended path before returning.

3. **`counter_wait` published the `Waiter` before the fiber parked.** A
   `counter_decrement` could grab a just-published `Waiter`, wake the
   fiber, and enqueue a resume before the fiber's own `fiber_switch` had
   saved its context → the resume `fiber_switch`'d into a stale context.
   And the `canceled`-bool cancel/wake handoff had a TOCTOU window
   (a decrement could see a published `Waiter` as "not canceled, target
   matches" before `counter_wait` decided to cancel → phantom resume into
   a still-running fiber). **Fix:** *switch-then-publish* — the fiber
   stashes a park request (`tl_set_pending_park`) and switches to the
   scheduler *first* (saving its context); the scheduler then publishes
   the `Waiter` (`counter_finish_park`), ABA-re-checks, and on a satisfied
   re-check resumes the fiber itself. Cancel/wake is now a single CAS on a
   3-state token `WaiterClaim {Pending, Canceled, Wakeup}`. A
   `Waiter::park_finalized` flag (set by `counter_finish_park` at its end,
   spun on by `counter_wait` after the switch) keeps the resumed fiber
   from racing ahead and freeing the counter / unwinding the `Waiter`
   while `counter_finish_park` is still mid-flight.

4. **`counter_decrement`'s "steal list → partition → re-push" raced
   concurrent decrements.** Decrement A steals a not-yet-satisfied
   `Waiter`, is about to re-push it; decrement C (the one to 0) runs its
   `exchange` in the gap, sees an empty list; A re-pushes onto a list
   nobody will ever drain again → lost wakeup → deadlock. **Fix:** since
   the job system only ever waits for the counter to reach 0,
   `counter_decrement` now touches the waiters list at *exactly one* point
   — the decrement that hits 0 — and only ever drains it, never partially
   rebuilds (no `put_back`). `counter_wait` / `counter_finish_park` assert
   `target == 0`.

**Verification:** disassembly (the trampoline no longer caches the TLS
base); ~9,000+ aggressive cross-thread-resume stress runs (4/6/8 worker
threads, deeply-nested `run_and_wait`) across release / `-O2 -g` asserts-on
/ `-O0` asserts-on builds — 0 failures (vs ~1-in-100-to-400 crashes before
the `tl_fiber` fix); full Linux ctest sweep (debug / debug-scalar / asan /
relwithdebinfo / release) green; in-tree regression test
`jobs: cross-thread fiber resume stress` (`[jobs][stress]`). Files (the fix):
`engine/jobs/src/worker_pool.{cpp,hpp}`, `engine/jobs/src/counter.{cpp,hpp}`,
`engine/jobs/CMakeLists.txt`, `tests/jobs/test_jobs.cpp`,
`tests/jobs/test_counter.cpp`. Session log:
`docs/sessions/2026-05-12-jobs-fiber-tls-hoist-fix.md`.

**Follow-up cleanups (2026-05-12, on the Windows dev box):** full 14→17-config
`scripts/full-sweep.ps1` (Win ×10 + Linux ×7, now incl. AVX2 codegen) PASS +
Windows `[jobs][stress]` hammered ×500/config + the old `crd-resources-tests`
streaming repro ×200 on `win-release` — all clean. With that confirmed: the
MSVC `/Od` on `worker_pool.cpp` + `fiber_init.cpp` was **dropped**
(`engine/jobs/CMakeLists.txt`); a plain-`sse2` SIMD lane was **added**
(`win-debug-sse2` / `linux-gcc-debug-sse2` config/build/test presets, CI matrix
entries, `full-sweep.ps1` lanes); the `CounterPool::release` debug assert-walk
over leftover `Waiter`s was **removed** (post the switch-then-publish protocol,
no Pending waiter can survive to `release()`, so the walk could only ever read
already-unwound `counter_wait()` stack frames); and `scripts/wsl-build.ps1` was
**hardened** — `linux-gcc-debug-sse2` added to its `[ValidateSet]`, and the
`& wsl.exe -- bash …` call moved out from under `$ErrorActionPreference='Stop'`
(PowerShell 5.1 was surfacing the inner bash's stderr — a benign CMake
`cmake_minimum_required` deprecation warning on a fresh `_deps` configure — as a
native-command error and killing the whole sweep; the v0e-post-mortem stderr
brittleness, now actually fixed; `$LASTEXITCODE` is still the real failure
signal). `docs/jobs/WINDOWS_VERIFICATION.md` is satisfied. This debt item is
fully closed.


### Phase 3.1 v0c `crd::math::deterministic` — debt paid 2026-05-10

The v0c original deferral list (5 items) was closed in a same-day v0c-debt-A pass. Status:

- ✅ **f64 overloads** — all 26 functions ship with Cephes f64 coefficients (sin/cos/tan/asin/acos/atan/atan2/exp/exp2/log/log2/log10/pow/expm1/log1p/sinh/cosh/tanh + rounding/abs/copysign/fmod). Proper Cephes Padé for atan (full f64 precision, ≤4 ulp).
- ✅ **`Vec4f` / `Vec8f` SIMD-batched overloads — full branchless implementation.** sin/cos/exp/log overloads ship for both Vec4f and Vec8f with the same Cephes coefficients as the scalar versions, evaluated branchlessly via `select()` for octant routing + bitwise-mask sign tracking. AVX2 builds emit 256-bit `vaddps/vmulps ymm` for the inner polynomial; SSE2/NEON builds use 128-bit lanes; scalar fallback gives correct lane-wise results. Lane-wise bit-exact parity with scalar is verified across all 12 configs.
- ✅ **Hyperbolic `sinh`/`cosh`/`tanh`** — f32 + f64.
- ✅ **`erf`/`erfc`/`gamma`/`lgamma`/`beta`** — f32 + f64 (f32 forwards to f64; f64 uses Cephes erf.c / erfc.c / gamma.c).
- ✅ **`expm1`/`log1p`** — f32 + f64 (Taylor band for tiny x, exp/log fallback otherwise).

Remaining v0c-related items (re-scoped):

#### Bessel + orthogonal polynomials (deferred to `crd-hesap-stats` v13)

`bessel_j0`/`j1`/`y0`/`y1`/`i0`/`i1`/`k0`/`k1` (Bessel of first/second/modified kind) and `legendre_p`/`hermite_h`/`chebyshev_t` (orthogonal polynomials) are statistics-module concerns — they belong in `crd-hesap-stats` v13 (Phase 3.1.6, ADR-0065) where they sit alongside distribution PDFs/CDFs that consume them. Cephes has battle-tested implementations; the cooker over there will copy them.

**Where referenced:**
- `engine/math/include/crd/math/deterministic.hpp` — doc-block at the top of the file points at this debt entry.
- `docs/sessions/2026-05-10-v0c-deterministic.md` — closing session log (original v0c).
- Pending: `docs/sessions/2026-05-10-v0c-debt-A-paydown.md` — debt-paydown session log.

---

### Phase 3.0 v1m Öbek system — three deferred follow-ups (2026-05-08)

The full Öbek system (ADR-0058) shipped across v1m1–v1m5b in twelve sub-slices. Three items were explicitly carved out as post-Phase-3.0 follow-ups so the v1m closure stayed focused.

1. **Hot-reload watcher with OCHN graph awareness** (v1m5c) — the öbek format already emits OCHN entries listing every transitive dependency (extends + nested) with FNV-1a 64 source-byte content hashes. What's missing: a filesystem watcher that consumes OCHN, detects upstream changes, and triggers transitive re-cook + atomic ResourceManager swap (matching the existing shader hot-reload pattern). Lands when filesystem-watching infrastructure is established (likely Phase 7 editor or earlier if a content workflow needs it).

2. **`obekc extract <source.obek.toml> --root <name> --output <new>` CLI tool** (v1m5c, ADR-0058 pillar 14 "Decompose") — extract a sub-graph rooted at a named entity into a new standalone `.obek.toml` file, with optional `--rewrite-source` to convert the original's inlined entities into a nested `obek = "..."` reference. Editor "make this a sub-prefab" operation. Needs its own binary entry point under `tools/`. Defer until the editor (Phase 7) or a real content workflow surfaces the need.

3. **InheritPolicy CoW: dense-buffer optimization** (v1m4b future) — v1m4b's CoW backend wastes `sizeof(component)` bytes per shared slot in the dense buffer (the bytes are unused for shared slots; only used after CoW write-break). For sizeof(component) >> sizeof(pool_idx), this dilutes the memory savings. A future optimization could allocate dense bytes lazily per-slot (e.g., a separate "owned slots only" dense buffer indexed by per-entity offset). Acceptable trade-off at v1m4b — pool-side dedup still gives N→1 sharing across instances, which is the dominant savings axis for the canonical "10k tree forest" workload.

**Where referenced:**
- `engine/scene/include/crd/scene/obek.hpp` — doc-block at the top of the file points at this debt entry.
- `docs/sessions/2026-05-08-scene-v1m5-revert-batch.md` — v1m closure session log.
- `docs/sessions/2026-05-08-scene-v1m4b-cow-backend.md` — pin #8 about wasted dense-buffer bytes.

---

### Phase 3.0 v1l cook_scene cooker — eight deferred follow-ups (2026-05-08)

`SceneCooker` + `scene_cooker_inline()` + `Transform`/six-relation built-in TOML readers + cooker-side propagation bake shipped in v1l. The authoring layer is in place; the following items are explicitly out of v1l scope.

1. **asset_cooker file-handler integration** — v1l ships the `SceneCooker` API but not the `.scene.toml` extension dispatcher. `tools/asset_cooker/src/cook_command.cpp`'s extension router does not yet route `.scene.toml` to `SceneCooker::cook_inline`. v1m (sandbox) or earliest content workflow will wire it; the API is ready and tested.

2. **Hierarchical entity addressing** — `[entity.player.weapon]` is rejected at cook time (test case in `test_scene_cooker.cpp`). A first-class child-as-nested-table syntax with cycle detection would simplify deep hierarchies; deferred to v1m+ once the sandbox surfaces a real authoring need.

3. **Per-instance prefab overrides** (v1k debt #5 reframed) — TOML `extends = "base.scene.toml"` with override blocks. The cooker is the right layer (instantiation-time merge). Reserved.

4. **Multi-file scene composition** — `[include = "level/region_a.scene.toml"]` recursive include with hot-reload-aware dependency tracking. Reserved for the streaming-load era (Phase 3.5+).

5. **Hot-reload of `.scene.toml`** — TOML watcher → recook → `SceneLoader.reload`. Same pattern as shader hot-reload but at the cooker layer. Reserved until the editor needs it.

6. **TOML schema migration** — when a component bumps its FourCC version, TOML migration tables let old `.scene.toml` files cook correctly without manual edits. Pairs with v1k debt #3 (binary-side migration).

7. **Compressed SCEN at the cooker** (v1k debt #7 picked up here) — CRDR supports zstd-compressed chunks (chunk-flag bit 0). v1l emits uncompressed. Multi-MB scenes will benefit; one-line flip in `SceneArtifactBuilder` once the cooker has size-based heuristics.

8. **Big-endian cooker output** (v1k debt #6 picked up here) — v1l SCEN is little-endian per CRDR. Cross-platform byte-order swap at cook time is a v1n+ concern.

**Where referenced:**
- `tools/asset_cooker/include/crd/cooker/scene_cooker.hpp` — doc-block points at this debt entry.
- `docs/sessions/2026-05-08-scene-v1l-cooker.md` — full session log with the propagation-bake fix and decisions.

---

### Phase 3.0 v1k SceneResource — seven deferred follow-ups (2026-05-07)

`SceneResource` + `SceneLoader` + `SceneArtifactBuilder` shipped in v1k. The persistence layer is in place; the following items are explicitly out of v1k scope. (Item #1 of the original eight closed by v1l on 2026-05-08; items #6 big-endian and #7 compressed SCEN repointed to the cooker layer in v1l's debt list above.)

1. **Streaming / incremental scene loading** — v1k loads-all-or-fail. Streaming visible-only entities (camera-frustum LOD, region-of-interest persistence) is Phase 3.5+. The current `SceneArtifactBuilder` filters at build time but the loader instantiates everything; partial-instantiation API is reserved.

2. **Schema migration** between SCEN versions — `kSceneSchemaVersion = 1` is fixed. v1n+ adds migration tables (v1 → v2 → ... transformer functions) once a layout change is needed. Pairs with v1l debt #6 (TOML-side migration).

3. **Entity-name lookup** post-load — finding a spawned entity by string name. Out of v1k scope; user-defined `Name` component or query-by-component is the path. v1m sandbox may want explicit name lookup; addressed there.

4. **Per-instance component overrides** — prefab+override pattern (instantiate scene, then override specific component values per entity). v1k loads verbatim. Now reframed as v1l debt #3 (cooker is the right layer).

5. **`World::mark_all_transforms_dirty()` helper** — convenience for callers loading a SCEN with stale world matrices who want propagation to re-derive. v1l's cooker bakes world matrices into SCEN bytes, so most callers no longer need this; the helper is still reserved if a use case appears.

**Where referenced:**
- `engine/scene/include/crd/scene/scene_resource.hpp` — doc-block points at this debt entry.
- `docs/sessions/2026-05-07-scene-v1k-scene-resource.md` — full session log with the eight decisions.
- `docs/sessions/2026-05-08-scene-v1l-cooker.md` — cooker session that closed item #1 and repointed #6/#7.

---

### Phase 3.0 v1j Transform — seven deferred follow-ups (2026-05-07)

`Transform` + `TransformPropagation` shipped in v1j with cross-domain robustness for games / robotics / aerospace / DAW. The following items are explicitly out of v1j scope; each has its own pickup phase or trigger condition.

1. **Polar decomposition for skewed Mat4** — `crd::math::to_trs` uses `from_mat3` on the post-scale-removal columns, which silently loses skew. True polar decomposition (SVD or iterative orthogonalisation) is reserved for a v1j+1 follow-up if a use case (CAD / mesh-import shear) appears. Documented in `to_trs`'s doc-block.

2. **TransformF64 (f64 precision) component + propagation system** — orbital / aerospace / atomic-resolution domains need f64 precision. Math layer already ships `crd::math::Transformd`. The v1j architecture supports it: register a `TransformF64` component + write a `TransformPropagationF64` `ISystem` that mirrors the f32 path. v1n's reserved-slot freeze test will verify the registration grammar accepts the custom type. v1k SceneLoader will accept it without changes.

3. **Parallel propagation** — single-threaded per ADR-0054. Phase 3.5 evolution once `par_each` over Query chunks lands. Per-subtree parallelism is straightforward (independent dirty roots → independent subtree DFS); each dirty root is one work-item.

4. **AttachedTo socket propagation** — Phase 3.2 (animation) ships an attachment-pose system that composes with TransformPropagation (sockets snap to bones).

5. **Per-system change tracking for `.changed<T>()`** — current ChangeDetect snapshot is "modified during current frame" (v1i pin). Cross-frame "what changed since my system last ran" needs per-system state. v1h+1 evolution.

6. **Auto-renormalize rotation policy** — v1j makes renormalize OFF-by-default. A registration trait (`AutoNormalizeRotation{}`) could opt-in per component. Reserved slot if accumulated drift becomes visible in real workloads.

7. **`set_rotation_look_at` direction convention** — current implementation uses (right, up, -forward) columns matching the right-handed convention. Some domains (aerospace yaw-pitch-roll) need (forward, right, up) variants. Reserved as a follow-up trait or alternative API if a domain needs the explicit convention.

**Where referenced:**
- `engine/scene/include/crd/scene/transform.hpp` — doc-block points at this debt entry for items 2 and 6.
- `engine/math/include/crd/math/quat.hpp` — `to_trs` doc-block points at item 1.
- `docs/sessions/2026-05-07-scene-v1j-transform-propagation.md` — full session log with the seven decisions.

---

### Memory allocator infrastructure (D-001 closed 2026-05-07)

`TlsfAllocator` (D-001-a) and `GrowablePoolAllocator` + ChunkAllocator-pooled (D-001-b) shipped. The v1c1 O(N) `ChunkAllocator::free` perf debt is closed — chunk allocate / free are now both O(1) via the GrowablePool's intrusive free-list. See `docs/sessions/2026-05-07-detour-D-001b-growable-pool.md` for the closing summary.

**Implicit-but-untracked debt also closed 2026-05-07 (allocator-audit Option C):** `ArchetypeGraph` was using `std::make_unique<Archetype>` — the only place in Phase 3.0 that bypassed the World's `IAllocator` chain. Closed by pooling Archetype structs via `GrowablePoolAllocator(slots_per_page = 32, parent = m_alloc)`. `test_world_tlsf.cpp` (5 cases) proves the deployment pattern: a `World` on a `TlsfAllocator` pool runs full ECS lifecycle and returns every byte to the pool on destruction. Session log: `docs/sessions/2026-05-07-archetype-pool-tlsf-world.md`.

### TLSF allocator — three deferred enhancements (D-001-a, 2026-05-07)

`TlsfAllocator` ships in production-grade form: arbitrary alignment, O(1) operations under ASan stress (1000 iterations × 16/32/64/128/256-byte alignments), `try_allocate` non-throwing path. Three enhancements are consciously deferred:

1. **Conte's 8-byte block-header overlap trick.** Saves 8 B per allocation by overlapping the next block's `prev_phys_block` field with the previous block's payload tail. Documented in `docs/sessions/2026-05-07-detour-D-001a-tlsf.md`. Layout change is high-risk; the 8-byte saving is marginal at engine scale (1000 allocations of 100 bytes each saves ~8 KB). Pick up if memory pressure ever justifies — likely never.

2. **32-bit pointer support.** Cerid CI is 64-bit. Constants (`kFlIndexMax = 32`, the `unsigned long long` cast in `fls_size`) assume 64-bit. Adding template parameterization on pointer width adds bug surface for zero current benefit. Pick up if a 32-bit embedded target ever appears.

3. **Multi-threaded TLSF.** `IAllocator` base class documents "not thread-safe by default; hand them out per-thread or wrap them yourself" — this is the engine-wide convention. Lock-based TLSF kills the O(1) latency claim; lock-free TLSF is research-tier (Marotta et al. 2018). The standard scaling pattern is per-thread arenas. Don't pick up — this isn't TLSF-specific debt; it's a project architecture decision.

**Where it's referenced:**
- `engine/memory/src/allocators/tlsf_allocator.cpp` — current implementation comments document each deferred item at the relevant code site.
- `docs/sessions/2026-05-07-detour-D-001a-tlsf.md` — full design rationale.

---

### Async GPU upload (`GpuUploader`) — design closed by ADR-0061; impl lands in v1o1+v1o2

**Status (2026-05-09):** **Design half closed.** ADR-0061 locks the contract: three layers, owned by three modules.
- `crd-rhi`: adds `Fence` + non-waiting `Queue::submit(cmd, fence)`.
- `crd-renderer`: adds `UploadHandle` + `GpuUploader::upload_mesh_async` / `upload_texture_async` + `PendingMeshUpload` component + `RenderUploadSystem` (RenderExtract phase).
- `crd-scene`: unchanged — already exposes `AsyncAwareIndex` + `query<...>().skip_pending<Renderable>()`.

**Implementation half:** lands as Phase 3.0 v1o1 (RHI fence) + v1o2 (UploadHandle plumbing + RenderUploadSystem). v1o3 is the sandbox integration that uses the async path — the first real consumer.

**Why it matters:** `GpuUploader::upload_mesh` / `upload_texture` today end with `device.graphics_queue().submit_and_wait(*cmd)` — a `vkQueueWaitIdle` on the main thread. For BoomBox-class assets (~10 MB GLB → ~30 MB raw mesh) that's a visible hitch even though the CPU-side load is already async (Phase 2.8 v1g). The sync entry points stay (some smokes/tests need immediate readiness); the async siblings join them.

**Reserved follow-ups (NOT blocking v1o):**
- `Device::transfer_queue()` — opportunistic dedicated transfer queue (Vulkan: separate `VK_QUEUE_TRANSFER_BIT` family); falls back to graphics when absent. Reserved for Phase 3.5+ when streaming pressure makes it worthwhile.
- Timeline semaphores — replace binary fences when a consumer needs multi-step ordering or batched waits.
- Streaming budget — at most N concurrent uploads; queue the rest. Phase 3.5+ when terrain/LOD streaming arrives.
- Async texture upload consumer — `PendingTextureUpload` sibling component. Lands when a real texture-streaming workload surfaces (likely Phase 3.5 IBL or 3.8 GPU-driven rendering).

**Where it's referenced:**
- `docs/decisions/0061-async-gpu-upload-contract.md` — full design + module ownership + caller pattern.
- `docs/phases/phase-3.0-scene-ecs.md::v1o` — implementation slicing.
- `engine/renderer/src/gpu_uploader.cpp` — current synchronous implementation; v1o1+v1o2 add the async siblings.
- `sandbox/src/sandbox_layer.cpp::try_finalize_pending_load()` — current consumer; v1o3 migrates to async.

---

## Material system v1 known gaps

`MaterialResource` as shipped in Phase 2.6 v1e is a loader proof-of-concept, not a production material
abstraction. Phase 2.7 v1c (ADR-0048) redesigns it as a full material system foundation: `MaterialTemplate`
+ `MaterialInstance` two-tier split, new MATR artifact format (INFO/PRMS/DFLT/PASS/PSOS/OPTS chunks),
`ParameterType` enum, `ShaderOption` system with inline functor, `SurfaceData` GLSL contract, `PassType` enum,
`MaterialDomain` (pulled forward from Phase 2.8), `RasterState` encoding in the artifact.

**Updated status (post-ADR-0048):**
- **Items 1–3 (artifact layer)** — Closed by Phase 2.7 v1c. The artifact format now carries: parameter
  schema (PRMS), defaults (DFLT), pass-keyed shaders (PASS), PSO state per pass (PSOS), shader options (OPTS).
- **Items 1–3 (GPU wiring)** — Phase 2.8 wires the artifact data to Vulkan pipeline compilation
  (per-material pipeline cache, multi-pass ForwardRenderPath, depth-only prepass).
- **Items 4–5** — Still deferred (item 4 → Phase 3.5 CSM; item 5 → Phase 3.7 post-FX or Phase 3.8 GPU-driven).
  No consumer exists yet.

### 1. Material parameters, texture slots, and full parameter system ✅ Closes Phase 2.7 v1c

**What was missing:** `MaterialResource` held two shader handles and nothing else. No parameter schema,
no texture slots, no shader variants, no material domain, no render-pass awareness.

**What v1c delivers:**
- `MaterialTemplate` (replaces `MaterialResource`): loaded from MATR artifact. Carries parameter schema
  (`Array<CookedParameter>` sorted by name_hash), default values blob, pass-keyed shader handles
  (`HashMap<PassType, ResourceHandle<ShaderResource>>`), PSO state per pass, shader option declarations.
- `MaterialInstance` (caller-owned, not in ResourceManager): mutable overrides atop a `MaterialTemplate`.
  `set_float` / `set_vec4` / `set_texture` write into a `values_blob`. `variant_for_pass(pass)` evaluates
  inline functor rules and returns the correct `ShaderResource` permutation.
- `ParameterType` enum: Float/Float2/Float3/Float4/Color/Bool/Int/Enum/Texture2D/TextureCube/Sampler.
- Cook-time SPIR-V reflection: spirv-reflect extracts UBO offsets; cooker emits `CookedParameter` entries
  sorted by name_hash for O(log N) binary search at bind time.
- Inline functor: `enables_option = "USE_NORMAL_MAP"` on a texture parameter — no C++ subclass needed.

### 2. PSO state in the material artifact — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1a

**Artifact layer (v1c):** `PSOS` chunk carries a `RasterState` per PassType (present_mask + RasterState
array). `RasterState`: AlphaMode, CullMode, FillMode, depth_test, depth_write, src/dst BlendMode.

**GPU wiring (Phase 2.8 v1a):** `ForwardRenderPath` reads `material->pso_states[pass_type]` and
incorporates it into the `GraphicsPipelineDesc` key. Per-material pipeline cache keyed by
`(VariantKey, RasterState)`. `ForwardRenderPath` skips non-`Surface` domain materials.

### 3. Shader variant awareness (VariantKey + pass-keyed variants) — ✅ Artifact layer closes Phase 2.7 v1c; GPU wiring Phase 2.8 v1b

**Artifact layer (v1c):** `PASS` chunk stores `HashMap<PassType, ResourceId>`. `OPTS` chunk stores shader
option declarations. `MaterialInstance::variant_for_pass(pass)` evaluates inline functor rules, constructs
a `VariantKey`, and returns the appropriate `ShaderResource` from `tmpl->pass_shaders[pass]`.

**GPU wiring (Phase 2.8 v1b):** `ForwardRenderPath` calls `mat_inst.variant_for_pass(DepthPrepass)` in
the depth prepass and `mat_inst.variant_for_pass(Forward)` in the color pass. Each pass uses the shader
selected by the instance, not a hardcoded vert+frag pair.

### 4. Descriptor layout — per-material bindings — Deferred Phase 3.5

**What's missing:** Nothing in `MaterialTemplate` drives descriptor set creation or layout for set 1+
(per-material bindings). The `VulkanDescriptorAllocator` and `MaterialBindGroup` (formerly `MaterialInstance`)
are wired to hardcoded layouts, not artifact-driven layouts.

**What to add (Phase 3.5):**
- `MaterialTemplate` carries enough reflected binding data to construct a `VkDescriptorSetLayout` at load
  time (or defer to the first bind).
- `MaterialResourceLoader` merges spirv-reflect results across pass shaders to build the per-material
  binding table.
- `MaterialBindGroup` is rebuilt from `MaterialTemplate` rather than from a manually-constructed layout.

**Why deferred:** No concrete consumer (texture arrays, multiple samplers) until CSM and area-light
materials land in Phase 3.5, and post-FX materials in Phase 3.7.

### 5. Additional shader stages — Deferred Phase 3.5+

**What's missing:** The PASS chunk stores vertex+fragment shader pairs (one `ShaderResource` per PassType).
There is no slot for compute, mesh, or task shaders. A compute-only material (post-FX, particle simulation)
cannot be expressed.

**What to add (Phase 3.5):**
- Extend `ShaderResource` to carry multiple stages (vertex/fragment/compute/mesh as a tagged union).
- Update `MaterialTemplate::pass_shaders` value type to `ResourceHandle<ShaderResource>` where each
  `ShaderResource` declares its own stage set (already possible via the existing shader mechanism).
- The PASS chunk format is already stage-agnostic (one ResourceId per PassType entry). Only the shader
  artifact format changes — the material artifact format is unaffected.

**Why deferred:** Compute and mesh shaders are Phase 5 concerns. The PASS chunk format already accommodates
them — the `ShaderResource` inside can carry any combination of stages.

---

**Updated execution plan:**
- Phase 2.7 v1c closes the artifact layer of items 1–3 (full material foundation: ADR-0048).
- Phase 2.8 wires items 2–3 to actual Vulkan pipeline compilation and multi-pass rendering.
- Items 4 and 5 remain open; deferred until consumers create real demand (item 4 → Phase 3.5 CSM /
  area lights; item 5 → Phase 3.7 post-FX compute / Phase 3.8 GPU-driven culling).

See `docs/phases/phase-2.7-asset-import.md`, `docs/phases/phase-2.8-material-completion.md`,
ADR-0044, ADR-0046, ADR-0048.

## Long-term deferred

- **Stress `[.soak]` nightly lane** (deferred by detour D-002, 2026-05-12) — `tests/stress/`
  ships four `[.soak]`-tagged tests (TLSF churn, freeze + parallel_for, `ConcurrentQueue`
  MPMC, `AtomicArray`) with deliberately huge iteration counts (catch a 1-in-10M torn-write
  / false-share that the bounded variants miss). They're Catch-`[.]`-hidden so CI `ctest`
  skips them; today they only run on demand via `crd-stress-tests "[.soak]"`. Wanted: a
  scheduled nightly CI job (and a place to add future `[.soak]` tests as v3–v6's primitives
  pick up consumers). Until then, run the soak suite manually before relying on a new
  consumer of these primitives.
- **Concurrent hash map** (deferred by detour D-002, 2026-05-12) — a split-ordered /
  Cliff-Click-style lock-free hash map is genuinely hard and should not be built
  speculatively. D-002 ships `crd::containers::ConcurrentQueue<T>` (MPMC) and
  `crd::containers::AtomicArray<T>` (bounded atomic-append); a concurrent map lands
  only when a concrete consumer demands it. Until then: per-fiber scratch maps + a
  serial merge, or a `ConcurrentQueue` of update-requests drained by one fiber.
- **Thread-safe / sharded global allocator** (deferred by detour D-002, 2026-05-12) —
  `TlsfAllocator` and the pool/linear/stack allocators are single-threaded-by-contract
  and will *not* get a lock bolted on. If a shared cross-fiber heap is ever needed it is
  a new sharded/thread-caching allocator (tcmalloc-style: per-thread free lists + a
  locked central heap), built then, not now. `RefCounted` objects whose final release can
  occur off the creating thread must be backed by a thread-safe allocator or a
  deferred-free queue.

- **Multi-viewport ImGui** — Vulkan multi-viewport has known rough edges.
  Single-viewport docking only until `crd-ui` ships (planned Phase 5+).
  At that point, game/editor surfaces move to `crd-ui`; ImGui stays debug-only
  and multi-viewport is no longer needed.

## GPU instancing (planned Phase 3.2)

v1h ships `draw_indexed(index_count, first_index, vertex_offset)` — non-instanced only
(`instance_count` hardwired to 1 in the Vulkan call). When instancing lands:

**RHI changes:**
- Add `draw_instanced(vertex_count, instance_count, first_vertex, first_instance)` to
  `CommandBuffer` (non-indexed instanced path).
- Add `draw_indexed_instanced(index_count, instance_count, first_index, vertex_offset, first_instance)`
  to `CommandBuffer` (indexed instanced path, mirrors `vkCmdDrawIndexed` fully).
- All four draw variants (`draw`, `draw_indexed`, `draw_instanced`, `draw_indexed_instanced`)
  coexist; `VulkanCommandBuffer` implements all four.

**Renderer changes:**
- `Renderable` and `DrawItem` gain `instance_count = 1` (default keeps backward compat).
- `ForwardRenderPath` dispatch logic: `instance_count == 1` → non-instanced path (no
  regression); `instance_count > 1` → instanced path.
- GPU instance data buffer (transforms, material indices) is a Phase 3 GPU scene buffer
  concern — `crd-resources` provides per-frame upload; `ForwardRenderPath` binds it as
  a storage buffer at set 0 binding 1 or via push constants for the base instance.

**When:** After Phase 3.1 (stable entity/transform storage in the scene system) ships and
a GPU instance data layout is frozen. Instancing without a stable instance buffer contract
produces nothing useful. Target: Phase 3.2.

**Do NOT prematurely add `instance_count` to `Renderable` / `DrawItem` before that point.**

## Renderer optimization backlog (post-v1g)

Intentionally deferred. These require the render path to be working end-to-end
before they pay off. Implement in order of demonstrated need, not in anticipation.

- **Transient image aliasing in the frame graph** — `FrameGraph::execute` currently
  creates transient images fresh each frame and destroys them on `reset()`. A proper
  aliasing pass would reuse GPU heap pages across mutually-exclusive transients,
  reducing VRAM by the sum of the largest non-overlapping resource sets. Prerequisite:
  lifetime analysis pass in `FrameGraph::build()`.

- **HDR render target** — `ForwardRenderPath` uses `B8G8R8A8Unorm` (LDR). Switch to
  `R16G16B16A16Sfloat` (scene linear HDR) and add a tone-map pass before the swapchain
  blit. Required before bloom, exposure, or any physically-based lighting integral.

- **Depth-only pipeline for the depth prepass** — `ForwardRenderPath` v1g reuses the
  full vertex+fragment pipeline in the depth prepass. A vertex-only pipeline (null
  fragment shader, `Format::Undefined` color, `Format::D32Sfloat` depth only) removes
  unnecessary fragment work during the prepass. Requires the per-variant pipeline cache
  to store `{depth_pipeline, color_pipeline}` pairs.

- **Async pipeline compilation** — `PipelineResolver::resolve_pipeline()` is currently
  synchronous. Slow variant compiles stall the main thread. Solution: compile on a job
  thread, return a "pending" sentinel, and render with a fallback pipeline until the
  real one is ready. Integrates with `crd-jobs` (Phase 2.5).

- **Bindless material system** — Current: one descriptor set per material instance per
  frame (set 1), allocated from the ring pool. Future: global bindless descriptor heap
  (one giant `DescriptorSet` with an array of all textures + material CBs), indexed
  via a per-draw material index in the push constants. Eliminates per-draw
  `vkCmdBindDescriptorSets` calls. Requires Vulkan device features: `descriptorIndexing`.

- **GPU-driven rendering** — CPU culling + indirect draw. Replace per-object draw calls
  with a compute dispatch that reads a scene buffer, outputs `VkDrawIndirectCommand`
  structs, and optionally writes a visible-object list. Requires: stable GPU scene buffer
  (Phase 3 scene system), `VkDrawIndirectCount` (Vulkan 1.2 core), and a GPU frustum
  cull shader. Significant throughput gain for dense scenes (> ~10k draws).

- **Split vertex streams** — Separate position-only VBO from full-attribute VBO. The
  depth prepass only needs positions; pulling the full vertex (UVs, normals, tangents)
  wastes memory bandwidth. Requires `DrawItem` to carry both VBOs and shader variants to
  declare which stream they consume.

## Cleared debt

- **Disabled-trace benchmark** (2026-05-03) — Replaced compile-time-eliminated
  `CRD_LOG_TRACE` call with `CRD_LOG_INFO` gated by `runtime_level = Error`. The
  benchmark now measures the runtime short-circuit cost in all build configurations.
- **Doxygen per-symbol comments in crd-core** (2026-05-03) — Added `///` docs to all
  symbols in `types.hpp` (14 aliases), `platform.hpp` (18 macros + 3 functions), and
  `assert.hpp` (2 type aliases + 4 functions + 4 macros).
- **No SPSC RingBuffer** (2026-05-03) — Added `SpscQueue<T>` in
  `engine/containers/include/crd/containers/spsc_queue.hpp`. Lock-free, cache-line
  padded head/tail atomics, wait-free push and pop. Tested single-threaded and with
  concurrent 1M-item producer/consumer.
- **No file watcher in crd-platform** (2026-05-03) — Added polling-based `FileWatcher`
  in `engine/platform/`. Uses `fs::last_modified_unix_seconds()` on each polled path.
  Handles add/remove by handle, fires callbacks synchronously in `poll()`.
- **Multi-viewport ImGui deferred** (2026-05-03) — Moved to "Long-term deferred" above.
  Will not land until `crd-ui` ships; ImGui stays debug-only forever.
