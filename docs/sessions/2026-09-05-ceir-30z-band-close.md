# 2026-09-05 — CEIR-30 band close (`ceir.dist` multi-device → sharded reduction; placement is semantic)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**CEIR-30 is CLOSED.** The §68/§103/§140 multi-device sharding band landed end-to-end on the hardware that exists (ONE RTX 4070
Ti SUPER + WSL2 passthrough): a device-free `ceir.dist` core (mesh + shard/all_reduce ops + a 9-kind verifier + the
Replicated/Sharded/Partial/Conflict propagation lattice + auto-materialization of all_reduce), the **Host (CPU) + Gpu (CUDA)**
§140 proof (the SAME lowered `[Reduce,Reduce,Elementwise]` plan runs all-Host and `[Host,Host,Gpu]` **bit-exact** on two RTX
runners, the only difference being WHERE each stage ran), authored placement (`transform.place_mesh` directive → loader → the
third per-stage ProviderClass producer + a committed `.ceir` asset), and the band-close transfer-cost bench that made the
census-predicted honest toy-dim loss quantitative. The proof of "placement is semantic, not glue": two real §24 memory domains
(`host`/`device_local`), a real host↔device transfer that costs a fixed per-crossing tax, and a free authored placement choice.

Detail → `docs/detours/D-007-ceir-tracker.md` (CEIR-30 rows). Bench board → `docs/bench/2026-09-05-ceir30-sharded-reduction-transfer-cost.md`.

## Slices (row per slice)

| slice | what | record |
|---|---|---|
| 30-0 | band-open census (advisor-verified; runnable proof = Host(CPU) + Gpu(CUDA), two real provider classes; two-CUDA-streams rejected) | tracker |
| 30a-1 | the `ceir.dist` dialect skeleton — `dist.mesh`/`dist.shard`/`dist.all_reduce` + `find_dist_misuse` (9 typed kinds) + the committed `dist_shard_reduce.ceir` anti-drift | device-free (MSVC + WSL gcc) |
| 30a-2 | sharding PROPAGATION — `propagate_sharding` (the Replicated/Sharded/Partial/Conflict lattice, elementwise meet, reduce-over-split→Partial, all_reduce completes, unknown→Conflict per §70) | device-free (MSVC + WSL gcc) |
| 30a-3 | MATERIALIZATION — `materialize_sharding` (auto-insert all_reduce where a Partial escapes, RAUW-all-then-restore; the 1-device identity byte-identical) | device-free (MSVC + WSL gcc) |
| 30b-1 | the HOST EXECUTOR `execute_tensor_pipeline_host` — the CPU mirror (synth_*→`kir::eval_cpu`, f32-FAITHFUL via per-op `round_dtype`); unsupported→UnresolvedKernel, no partial write | device-free (MSVC + WSL gcc, 4/4) |
| 30b-2a | `plan_transfers` + `stage_class_from_partition` — the PURE placement→transfer pass (a ProviderClass per stage → the host↔device transfers the def-use edges imply; Host-born externals + the planned Output readback; aliases→landlord) | device-free (MSVC + WSL gcc) |
| 30b-2b-1 | the device-free Host VizDispatch(relu) exit — a caller `HostKernelResolveFn` loads the authored `.ckir` → `eval_cpu_kernel` (bit-exact vs the CUDA relu by construction) | device-free (MSVC + WSL gcc) |
| 30b-2b-2a | the engine `execute_two_class` runner (transfers computed internally from stage_class; per-stage slice; profile complete) + `slice_plan` hoisted (ADL) — DEVICE-FREE mock gate (bit-exact under [G,H,H,H,G]/reverse/all-Host/all-Gpu, log==plan_transfers, aliased landlord) | device-free (MSVC + WSL gcc) |
| 30b-2b-2b | the §140 REAL-DOMAIN proof — the CUDA harness + the three-way gate (the sandwich Host+CUDA bit-exact vs all-Gpu vs all-Host vs oracle) + the resolver refactor + the fixture hoist (one oracle body) | device (Win RTX #4848 + WSL2 RTX) |
| 30b-3a | `lower_sharded_reduction` — the reduction→per-rank-plan LOWERING + placement producer (`rank_lineage`), device-free: E shard declares + E tagged reduces + an elementwise combine tree, RAUW+erase in-place, the 1-device byte-identical identity, refuse-on-Conflict | device-free (#896, MSVC + WSL gcc) |
| 30b-3b | the SAME plan through `execute_two_class` on real CUDA — placement `[Host,Host,Gpu]` (both reduces on Host, the all-reduce COMBINE on CUDA), bit-exact vs all-Host + the two-stage oracle; `n_pipes==1`/`n_xfer==3`/profile | device (Win RTX #4849 + WSL2 RTX #4696) |
| 30c-1 | the `transform.place_mesh` directive (op via opgen) + the `seen_place` DuplicateDirective guard + the `placement_from_transform` loader (mesh-keyed, axis-0 count-validated, typed rejects each asserted by IDENTITY incl. `op != nullptr`) + `stage_class_from_placement` (the THIRD producer) | device-free (#897, MSVC + WSL gcc) |
| 30c-2a | the committed `assets/ceir/place_mesh_host_host_gpu.ceir` + its device-free reading gate (bootstrap-via-print, anti-drift through the PRINTER, roundtrip, `dist::find_dist_misuse` dist-verify-first) + the shared-slurp hoist | device-free (#898, MSVC + WSL gcc) |
| 30c-2b | the CUDA end-to-end — the LOADED placement DRIVES `execute_two_class` (the hand-written `stage_class_from_partition` stays as the PARITY reference, REQUIRE-aborts on drift); closes the 30c-1 fold at the CUDA site | device (Win RTX #4851 + WSL2 RTX #4698) |
| 30z-1 | the band-close BENCH — the §140 sharded-reduction TRANSFER-COST decomposition (A all-Host / B `[Host,Host,Gpu]` / C the transfer floor) + the board | bench (Win RTX #4853 + WSL2 RTX #4700) |
| 30z-2 | close: census reconcile + deferral ledger + 3 scar clauses + flip ✅ | this record |

## Device / bench boards (row per device)

Full numbers → `docs/bench/2026-09-05-ceir30-sharded-reduction-transfer-cost.md` (do not restate here — SANITY: one home for
numbers). Correctness on-device: **30b-2b-2b / 30b-3b / 30c-2b** all bit-exact vs the two-stage f32 oracle AND the all-Host run on
**Windows RTX 4070 Ti SUPER (CUDA 13.3) + WSL2 RTX 4070 Ti SUPER (CUDA 12.0)**. Bench verdict (30z-1): the cross-domain placement
tax is a FIXED per-crossing cost (arm C ~constant in rows, `tax(B−A) ≈ C_xferfloor ≫ combine_gpu`), amortized by O(rows×cols)
shard compute — `B/A` collapses 345×→1.10× (Win) / 1021×→1.12× (WSL2) from `[8,4]` to `[4096,512]`. The census predicted this
honest toy-dim loss at 30-0, and 30z-1 measured exactly it — contrast 29z, where the census's bench-column sketch was wrong.

## RESOLVED (not deferred — the 29z "DISSOLVED" analog)

Three 30-0 census items were RESOLVED by the band, struck in place in the tracker (SUPERSEDED rule), NOT carried to the ledger:

- **the F32-eval-mode / F64-tier caveat → RESOLVED at 30b-1.** The census FACT ("eval is the F64 reference tier, ckir.hpp:31 —
  a host-vs-cuda bit-exact gate needs an F32-rounding eval mode OR a tolerance") was imprecise: `kir::eval_cpu` ROUNDS every
  elementary op to the node dtype (`round_dtype`, ckir_eval.hpp:48-54/240-242/275-278), so an F32 graph is bit-exact vs a naive
  f32 GPU kernel — the SAME oracle every device backend proves against IS the Host executor. The §140 gate is `==`; neither a
  separate eval MODE nor a tolerance was needed.
- **the boundary-transfer `StageKind::Transfer` plan stage → DISSOLVED (the 29b-2b IExecutionProvider shape).** The 29c-1 /
  29-0 trigger FIRED (a Host provider, `memory_domain="host"`, executes alongside CUDA), but NO `StageKind::Transfer` plan-stage
  member was ever added (the plan `StageKind` enum, tensor_pipeline.hpp:53, has no Transfer; `LoweredKind::Transfer` at
  lower.hpp:25 is the unrelated 13b command-movement kind). The cross-domain movement became `plan_transfers` (a DERIVED
  transfer list from the per-stage ProviderClass, 30b-2a) + `execute_two_class`'s upload/readback CALLBACKS (30b-2b). Struck at
  the 30-0 fact-(5) site and the 29z close doc's ledger bullet.
- **the census `emit_reduce_cuda`/`emit_broadcast_nd_cuda` "absent" claim → CORRECTED at 30b-3b.** `emit_reduce_cuda` EXISTS
  (ckir_cuda.hpp:712) — it is trailing-axis-only + takes a 2-scalar push incompatible with the single-blob CUDA dispatch, so it
  is not WIRED for the §140 axis-0 reduce, but "not wired" ≠ "absent". The census grepped the CONSUMER (`resolve_stage`) and
  inferred the producer's absence — the inverse of the grep-the-consumer rule (now a clause in that memory). reduce-on-CUDA is
  its own deferred slice (below), not a §140 blocker (§140's collective is the all-reduce, which runs on CUDA as an elementwise).

## Deferral ledger (each with a trigger — no speculative build)

- **a real 2-GPU mesh** → a second PHYSICAL GPU **AND** the reduce-on-CUDA contract (halving `A` means the per-rank reduces run
  in PARALLEL on two devices, so the rank-1 reduce must run ON its device). The 30z-1 board quantified the payoff: at `[4096,512]`
  the Host reduce is ~28 ms serialized; two devices in parallel → `A` halves while the ~1.7 ms tax stays fixed → predicted
  `B/A ≈ 0.55–0.6`, a WIN (conservative — a GPU reduce is faster than the CPU one).
- **NCCL / MPI / remote / cluster providers** → a cluster (single-box has none).
- **§103 `reshard`** → a consumer that changes a tensor's sharding mid-program (the propagation lattice models it; no consumer yet).
- **the §68 collective long-tail** (broadcast / all-gather / reduce-scatter / barrier / collective groups) → each its first consumer.
- **reduce-on-CUDA** (`emit_reduce_cuda` is trailing-axis-only + its 2-scalar push is the cross-launch-site CONTRACT shared by the
  ceir-gpu blob + kir-cuda×2 + kir-hip scalars) → a CUDA shard whose per-rank compute is an outer/strided-axis reduce.
- **the riders re-homed from the 29c-1 `Transfer` strike:** `is_memory_domain` string validation of the domain attrs + the
  run·CKIR·run double-transfer → a provider whose domain strings need validating / a partition spanning two API memory domains.
- **the `mean` collective-fn** (`collective_fn_in` = {sum,prod,max,min}, deliberately MINUS mean — mean needs a post-scale
  materialization the graph tier lacks) → a consumer needing `all_reduce(mean)`. ⛔ do NOT "fix" by hoisting `tensor.cpp`'s
  `fn_in`: the divergence is by design (30a-1).
- **`peer_copy` + a mesh rank > 1** (a multi-axis mesh — the 30c-1 `MultiAxisMesh` reject names a `mesh_axis` attr forward) →
  their first consumers.
- **the 29-0 non-CUDA-graph-API items** carried forward UNCHANGED — **but WITHOUT §103 multi-device sharding, which LANDED here**
  (the sharding MACHINERY is done; only the >1-GPU hardware remains): D3D Compute Graph Compiler / MLIR Programs · VK_ARM_data_graph ·
  NPU · D3D12 Work Graphs · CUDA device-graph launch · Metal ICB · cross-API interop · autotuned provider choice.

## Scars (3 clauses added to EXISTING memory homes — no new files)

- `scars_ceir_ir.md` (Passes/verifiers) — **a lowering that STRIPS an asset ENDS its consumers' window:**
  `lower_sharded_reduction` calls `strip_dead_meshes`, so every mesh consumer (`placement_from_transform`, `find_dist_misuse`)
  must run after `materialize_sharding` but BEFORE `lower_sharded_reduction`. Sequence the verify/place/read of X before the strip.
- `feedback_bench_arms_must_match_wait_and_bracket_structure` — **the correctness GATE that guards a bench must loop every
  compared arm the SAME N:** 30z-1's first draft ran arm B ×10 and arm A ×1 before the bit-exact compare, while the board claimed
  "10 back-to-back" for both. Loop every arm the claim names the same N; make the board match the code.
- `feedback_coverage_inventory_grep_the_consumer_not_the_node` — **the INVERSE: an "X is ABSENT" claim needs X's DEFINITION-site
  grep, not the consumer's.** The 30-0 census inferred `emit_reduce_cuda` absent by grepping the consumer; 30b-3b found it alive.
  Consumer-grep answers "is it reachable"; a negative producer claim is a definition question.

## Uncommitted batch (the user commits — NO AI co-author trailer)

CEIR-30 rides the ongoing CEIR-26→30 working-tree batch (last commit `c35548a "working on CEIR-25"`). The CEIR-30 additions/mods
(see each tracker row's UNCOMMITTED-BATCH line for the authoritative per-slice list): `engine/ceir/ops/dist.ceirop.toml` +
`engine/ceir/generated/crd/ceir/gen/dist_ops.*` + `engine/ceir/include/crd/ceir/dist.hpp` + `engine/ceir/src/dist.cpp` (the dist
dialect + verifier), `engine/ceir/ops/transform.ceirop.toml` + `engine/ceir/src/transform.cpp` +
`engine/ceir/generated/crd/ceir/gen/transform_ops.*` (the `place_mesh` directive + `seen_place` guard),
`engine/ceir-gpu/{include,src}/…/sharding.*` (propagate/materialize/`lower_sharded_reduction` + the hoisted `gather_meshes`/
`resolve_mesh`/`mesh_extent`), `…/partition_ml.*` (`Placement`/`PlacementKind`/`placement_from_transform`), `…/tensor_pipeline_exec.*`
(`execute_tensor_pipeline_host` + `plan_transfers` + `stage_class_from_partition` + `execute_two_class` + `slice_plan` +
`stage_class_from_placement`), `…/tensor_pipeline.cpp` (the `resource.export` skip), `assets/ceir/{dist_shard_reduce,place_mesh_host_host_gpu}.ceir`,
`tests/gpu-shared/{ckir_asset_resolve,two_class_fixture,ceir_asset_slurp}.hpp` (NEW shared test headers), the ceir / ceir-gpu /
ceir-gpu-cuda gates + CMakeLists lines, and the bench board `docs/bench/2026-09-05-ceir30-sharded-reduction-transfer-cost.md` +
its `docs/bench/README.md` index line. `git status` is authoritative; the per-slice tracker rows carry the definitive per-slice files.

## Proposed commit message (Conventional Commits — NO AI co-author trailer, per CLAUDE.md/AGENTS.md)

```
feat(ceir): CEIR-30 ceir.dist multi-device — sharded reduction, placement is semantic

Device-free ceir.dist core (mesh + shard/all_reduce ops + 9-kind verifier +
Replicated/Sharded/Partial/Conflict propagation + auto-materialized all_reduce)
+ the Host(CPU ckir_synth/eval) + Gpu(CUDA) §140 proof: the SAME lowered
[Reduce,Reduce,Elementwise] plan runs all-Host and [Host,Host,Gpu] bit-exact on
two RTX runners, differing only in placement. Adds execute_tensor_pipeline_host
(f32-faithful CPU mirror), plan_transfers/stage_class_from_partition,
execute_two_class, lower_sharded_reduction, and the transform.place_mesh authored
placement directive + committed .ceir asset. Band-close bench decomposes the
cross-domain transfer tax (fixed per-crossing, amortized by shard compute).
```
