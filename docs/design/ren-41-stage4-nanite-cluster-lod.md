# REN-41 Stage 4 — Nanite cluster-LOD renderer integration (design contract)

> The implementation contract for the REN-41 remainder. Row: **D-007 REN-41** (links here). Live state:
> `context.md`. Campaign dossier: `docs/research/2026-08-02-visual-frontier-plan.md` (§"Stage 4"). Algorithm/data
> pipeline: **REN-40-I, CLOSED** (56/56). This slice is the RENDERER SEAM that makes 40-I draw a frame.

## Why this exists, and where it is used

REN-41's frontier bundle has two geometry tiers, and they solve OPPOSITE regimes:

- **Discrete-LOD chain + octahedral impostors (40-C, done)** — the right tool when the scene is **instance-count
  bound**: a million instances of a ~6 K-triangle mesh. 40-C's arithmetic: at 1M instances a 2-triangle impostor
  beats the cluster DAG's coarsest level **64×**, which is why UE ships impostors/HLOD *alongside* Nanite.
- **Nanite cluster-LOD (this slice)** — the right tool when the scene is **triangle-count bound**: ONE (or a few)
  meshes each carrying **millions of triangles**. The cluster DAG draws only a screen-resolution triangle budget
  (~10–20 M for a 1080p frame) no matter how dense the source mesh is, and the LOD is **continuous by
  construction** — a cut through the DAG, no dither, no pop.

**Where Cerid uses it** (this is a general-purpose engine, not only games — `docs/PRINCIPLES.md`):
- **Cinematic / film** — scanned or ZBrush-sculpted hero assets (heads, creatures, props) authored at full
  density and drawn without a manual LOD budget.
- **CAD / medical / scientific** — dense scanned or simulated meshes (photogrammetry, CT/MRI iso-surfaces, marching-
  cubes output from B19-C2) where decimating by hand is not an option and the data IS the deliverable.
- **Games** — the hero/environment tier UE5 uses Nanite for: cliffs, statues, kit-bashed sets.

It is the **"unlimited geometric detail" tier**: author/scan at any density, draw a constant budget. Composed with
TAA (Stage 2, done) it resolves the residual sub-pixel AA the far field still carries. Finishing it **closes
REN-41** and gives Cerid the full UE5-class geometry pipeline — Nanite ⊕ impostors/HLOD ⊕ TAA.

## Reuse audit (SANITY #8 — grep the engine first; per gap: wiring vs new work, with evidence)

**✅ ALREADY BUILT — do NOT rebuild:**

- **The whole algorithm + GPU-packed data (REN-40-I, `crd-geometry-mesh-processing`, 56/56).**
  `cook_cluster_dag(positions, indices, opts, out, scratch)` → `packed_clusters` (**10 u32/cluster**: vtx_off,
  tri_off, vtx8|tri8|level16, error, parent_error, center.xyz, radius) + `packed_bvh` (**8 u32/node**:
  center.xyz, radius, max_error, min_parent_error, left, right) + `cluster_vertices` + `cluster_triangles_packed`
  (4 u8 local indices/u32) + `positions` (`cluster_dag_cook.hpp:12-33`). `kClusterGpuWords=10`, `kBvhNodeGpuWords=8`.
- **The selection reference** — `select_clusters_bvh(packed_clusters, packed_bvh, params{error_threshold,
  proj_factor, camera_pos}, out_selected, max, scratch)` and `select_clusters_flat`, "CPU-side selection that
  mirrors the GPU compute kernel EXACTLY" (`cluster_select.hpp:6`). Criterion: `parent_error > τ·dist·proj` AND
  `error ≤ τ·dist·proj`. **This is the oracle the device kernel gates against** (parity-by-derivation, the 40-A pattern).
- **The unpack reference** — `unpack_selected_clusters(...)` → positions + index triples, "the CPU reference for
  the mesh shader" (`cluster_unpack.hpp:6-12`). **This is the oracle the mesh shader gates against.**
- **The mesh/task shader stack (REN-38-F6/B4).** `IRasterContext::create_task_mesh_program(task, mesh, fragment)`
  + `draw_mesh(..., group_count)` (`raster_context.hpp:456,471`); the `raster.mesh` frame-pass kind
  (`scene_mesh.frame.toml`); `SceneRenderer::ensure_mesh_program()` cooks `scene_task.crdv`+`scene_meshlet.crdv`
  (`scene_renderer.cpp:1206`). The **CKIR mesh emitter is real**: GLSL `KStage::Task`→`EmitMeshTasksEXT` + a fixed
  4-field `TaskPayload` (`ckir_glsl.hpp:1631,1717`); `KStage::Mesh`→`SetMeshOutputsEXT` + per-vertex
  `gl_MeshVerticesEXT[idx].gl_Position` + per-primitive `gl_PrimitiveTriangleIndicesEXT[idx]` (`ckir_glsl.hpp:1721-1894`);
  HLSL twin present (`ckir_hlsl.hpp` matches the same builtins).
- **The compute-pass + GPU-driven vocabulary (40-A).** The authored `[cull]` kernels write a compacted list + a
  device count into a storage buffer, and the frame graph orders `reset`→`cull`→draw — the exact shape
  `cluster_select`→`mesh_draw` needs. Reuse the vocabulary, not new machinery.
- **The frame graph** builds compute→raster.mesh on both backends (my cycle-detector fix this session keeps it honest).

**⚠ GENUINELY MISSING — the seam this slice builds:**

1. **A GPU `cluster_select` compute kernel** — port `select_clusters_bvh` to a CKIR compute kernel writing the
   selected-cluster list + count on device. *New kernel, but the CPU reference is exact and the cull kernels are the
   authoring template.* Wiring-heavy, low risk.
2. **REAL cluster task + mesh programs** — the shipped `scene_task.crdv`/`scene_meshlet.crdv` are **skeletons**
   (`scene_task.crdv`: just `workgroup=32, emit=2`; no body) — F6 only proved "a mesh shader renders *something*".
   The real body (task reads the selected list → emits a mesh workgroup per cluster; mesh reads the packed cluster,
   transforms each vertex by `view_proj`, emits ≤128 tri index triples) is **new authoring**. **⛔ KEY RISK — de-risk
   FIRST:** whether the `.crdv` mesh authoring can EXPRESS the unpack body (storage_load of the packed buffer +
   per-`gl_LocalInvocationIndex` position/prim), or whether the vertex-cook mesh path needs extending. S4-0 answers this.
3. **Scene-renderer plumbing** — cook_cluster_dag a high-poly mesh at load; upload the 5 packed arrays to a device
   buffer with a declared header (offsets, counts) like the group buffer; a **`MeshRenderer` cluster-vs-discrete
   route flag** (an optional component, so the discrete path costs nothing — the 40-C `MeshLodOverride` precedent).
4. **Authored frame passes** `cluster_select` (compute) → `mesh_draw` (raster.mesh), both backends.

## Increments (each lands with its gate; sequenced to de-risk the mesh body first)

- **S4-0 — MESH-BODY SPIKE (de-risk).** Author a real cluster task+mesh program that unpacks ONE cluster from a
  packed buffer (hardcoded selection = {0}) and renders its ≤128 tris; the mesh reads `packed_clusters` +
  `cluster_vertices` + `cluster_triangles_packed` + `positions`, transforms by a header `view_proj`.
  **Gate:** pixels match the `unpack_selected_clusters` CPU reference for that cluster, on **Vulkan AND DX12** (⛔ the
  mesh-shader-scars-SILENT rule: one EXECUTING gate per backend — the DX12 `m_list6` view + AS→MS payload PSO
  contract are only exercised by a real DispatchMesh). If `.crdv` cannot express the body, this increment extends
  the vertex-cook mesh authoring (and says so) — that is the whole point of doing it first.
- **S4-1 — packed cluster buffer + route flag.** The device buffer layout (header + the 5 arrays), cook_cluster_dag
  at load, the `MeshRenderer` route flag. **Gate:** the on-device unpack of ALL leaves == the CPU unpack (triangle
  count == original mesh — 40-I's own I-8 gate, now on device).
- **S4-2 — GPU cluster_select.** The CKIR compute kernel + the authored `cluster_select` pass writing the selected
  list + device count. **Gate:** device selection == `select_clusters_bvh` — SAME count AND SAME set — at several
  distances, both backends (the 40-A parity-by-derivation gate; "not-everything / not-nothing" bounds so a
  select-all or select-none kernel cannot pass).
- **S4-3 — the full path.** Task reads the selected list (count from S4-2's device buffer); mesh unpacks each
  selected cluster; authored `cluster_select`→`mesh_draw` frame. **Gate:** a high-poly mesh renders; **triangle
  count scales with screen size** (MEASURED, not asserted — the count read back per distance); pixel parity vs a
  full-resolution reference at close range; both backends; 0 validation errors incl. a GPU-AV soak.
- **S4-4 — scene integration + close.** A hero mesh routed through the cluster path alongside the discrete-LOD grid;
  TAA resolves residual sub-pixel AA. **Gate (DoD):** continuous, pop-free LOD from any distance; both backends;
  screenshots at near/far; a bench board (`docs/bench/`); proposed commit.

## Open design decisions (resolve in-increment, recorded here so they are decisions not drift)

- **Mesh-draw workgroup count from a DEVICE count.** `cluster_select` writes `selected_count` to device memory, but
  `draw_mesh(group_count)` takes a CPU count. Two ways: (a) `DispatchMeshIndirect` (device count — check backend
  support) or (b) dispatch a CONSERVATIVE max task-workgroup count and have the task shader early-out beyond the
  device count (the GPU-driven idiom, no new verb). **Lean (b)** unless indirect-dispatch-mesh is already supported.
- **Task→mesh fan-out.** One task workgroup covers `emit` clusters via the 4-field `TaskPayload` (cluster indices);
  pick `emit` so the payload carries the cluster id(s) for the mesh workgroups it launches.
- **Vertex dedup within a cluster.** `cluster_vertices` already deduplicates per cluster (≤256 verts); the mesh
  shader emits `vertex_count` verts then `triangle_count` prims — a direct map to `SetMeshOutputsEXT`.

## Acceptance criteria (the whole slice)

A high-poly mesh routed through the cluster path shows continuous, pop-free LOD from any distance; triangle count
scales with screen size (measured); identical on Vulkan and DX12; TAA resolves residual sub-pixel AA; 0 validation
errors; a bench board captured at slice close.

## Non-goals (explicit — an omission is a decision)

- **Virtualized STREAMING** — in-memory clusters only (40-I stated this; the DAG is fully resident).
- **Per-cluster material variation** — one material per hero mesh (material×cluster is a later slice).
- **Software rasterization of sub-pixel triangles** — HW mesh-shader path only; the visbuffer SW-raster tier
  (Nanite's other half, for pixel-size triangles) is a separate REN slice, not this one.
- **Auto-routing heuristic** — the cluster-vs-discrete choice is an authored flag this slice; a cost-model
  auto-picker is future work.

## Named risks (with the scar each maps to)

- **Mesh shaders fail SILENTLY on a backend** — `feedback_mesh_shader_device_scars` (the DX12 `m_list6` stale-view +
  VRS drop) and `feedback_as_ms_payload_contract_dx12_pso`. Mitigation: S4-0's executing gate PER backend, first.
- **Cook-only gates ship device-impossible programs** — `feedback_cook_only_gates_ship_device_impossible_programs`,
  `feedback_shader_capability_needs_device_feature_run_validation`. Every increment runs on a device, not just cooks.
- **The `.crdv` mesh body may not be expressible** — de-risked by S4-0 before any downstream increment depends on it.
- **Indirect draw count contract** — `feedback_indirect_draw_verbs_must_push_the_drawindex_row` if S4-3 goes indirect.
