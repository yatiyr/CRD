# 2026-09-05 — CEIR-29 band close (native graph providers: the §70 partitioner across provider classes)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

**CEIR-29 is CLOSED.** The §70/§102/§69 native-graph partitioner landed end-to-end: a device-free partitioner core (provider
classes, maximal-subgraph greedy-grow, authored pinning + class constraints), the runnable **CUDA-Graphs** launch-graph provider
proven bit-exact on two real RTX devices, the two-class boundary captured on-device, and a launch-vs-N-dispatch bench. The
runnable §70 provider is CUDA Graphs (launch-graph ⇒ bit-exact vs the CKIR/stream fallback by construction); the semantic-engine
graph APIs (D3D Compute Graph Compiler, VK_ARM_data_graph, NPU) are deferred with triggers.

Detail → `docs/detours/D-007-ceir-tracker.md` (CEIR-29 rows). Bench board → `docs/bench/2026-09-05-ceir29-cuda-graphs-launch.md`.

## Slices (row per slice)

| slice | what | record |
|---|---|---|
| 29-0 | band-open census (advisor-verified; runnable provider = CUDA Graphs; seam = MlProvider input + IExecutionProvider execution) | tracker |
| 29a-1 | `ProviderClass` enum + `MlProvider` descriptor fields {provider_class, memory_domain(§24 string), determinism}; `is_memory_domain` extracted | #873 |
| 29a-2 | maximal-subgraph greedy-grow (`claims_subgraphs` + monotonic `subgraph` id) | #818/#871 |
| 29a-3a | authored provider PIN — a preference among claimers, never a force | #819/#872 |
| 29a-3b-1 | `transform.assign_provider` directive + `provider_from_transform` loader (name→index, typed rejects) | #876 |
| 29a-3b-2 | committed `assign_provider_coopvec.ceir` asset (canonical, anti-drift-through-printer, end-to-end pin) | #877 |
| 29b-1 | the CUDA tensor-pipeline DEVICE GATE (expanded ml.mlp gemm→relu→gemm, bit-exact vs the CPU oracle, Win RTX + WSL2 RTX); +2 CUDA-backend bug fixes (emit_contract_cuda uint4 push; the test target's CRD_REPO_DIR) | device |
| 29b-2a | CUDA-Graphs capture MECHANISM (begin_capture / end_capture[instantiate-once] / launch[many]; bit-exact + deterministic replay) | device |
| 29b-2b | the `cuda_graphs` provider descriptor + the SEAM reconcile (launch-graph = plan launch-mode, NOT an IExecutionProvider compile→plan implementor) | #878 |
| 29c-1 | partition-aware plan (`PlanStage.provider` + `plan_tensor_pipeline_partitioned` + expand→ml lineage; boundary-transfer STAGE untriggered/name-forward) | #879 |
| 29c-2 | the two-class boundary on a REAL device (capture ONLY the cuda_graphs run, flanking fallback gemms eager, bit-exact; first live 26f alias on CUDA) | device |
| 29c-3a | the `provider_class` HARD-FILTER composed with the pin (filter outer, pin inner) | #880 |
| 29c-3b | the authored `transform.constrain_provider_class` directive + the opgen C++-keyword sanitizer it surfaced | #881 |
| 29z-1 | the launch-vs-N-dispatch BENCH + the `CudaComputeContext::enqueue` split it forced | bench |
| 29z-2 | close: deferral ledger + 2 scars + flip ✅ | this record |

## Device / bench boards (row per device)

Full numbers → `docs/bench/2026-09-05-ceir29-cuda-graphs-launch.md` (do not restate here — SANITY: one home for numbers).
Correctness on-device: **29b-1 / 29b-2a / 29c-2** all bit-exact vs the CPU MLP oracle on **Windows RTX 4070 Ti SUPER (CUDA 13.3)
+ WSL2 RTX 4070 Ti SUPER (CUDA 12.0)**; the `enqueue` single-submit two-class path is CI-gated bit-exact in 29c-2 on both
(Win #4824 + WSL #4671). Bench verdict: the CUDA-Graphs win is the CPU SUBMIT PATH — the two-class sandwich wins on both
devices (Win 1.36–2.24×, WSL 1.13–1.57×, growing with dispatch count); GPU-time is an honest toy-dim loss (cuGraphLaunch fixed
overhead).

## Deferral ledger (each with a trigger — no speculative build)

- **`emit_permute_cuda` / attention-on-CUDA** (29b-1) → a CUDA consumer needing a transpose/permute (attention CANNOT run on
  CUDA today; the 29c-2 fixture is a gemm sandwich for this reason).
- **fused-GemmRelu on CUDA** (29b-1) → `emit_contract_cuda` does not unwrap the fused Max epilogue; trigger: a fuse=true CUDA
  pipeline.
- ~~**cross-domain `StageKind::Transfer`** (29c-1) → a provider whose `memory_domain ≠ device_local` executes alongside the
  fallback (one CUDA domain never fires it).~~ **[RECONCILED at CEIR-30 (30b-2a/30b-2b) — the trigger fired (a Host provider,
  `memory_domain="host"`), but there is NO `StageKind::Transfer` plan stage: the movement dissolved into `plan_transfers` (a
  derived transfer list) + `execute_two_class` callbacks. See the CEIR-30z close doc's RESOLVED section.]** Riders that did NOT
  land ride to the CEIR-30 ledger: `is_memory_domain` validation of the domain strings + the run·CKIR·run double-transfer (both
  named at 29a-2/29a-3a).
- **`DeterminismClass` as a filter axis** (29c-3a) → an authored "BitExact or fallback" constraint (the natural next filter
  after `provider_class`).
- **per-op §71 directive targeting** (27a/29a) → two sites wanting DIFFERENT decisions (now THREE program-global directives —
  fuse/share_storage/assign_provider/constrain_provider_class — would each need it).
- **the per-bracket-floor GPU control + larger-fixture parity** (29z-1, NEW) → a captured run whose kernel time exceeds
  ~50 µs, where `cuGraphLaunch`'s fixed cost stops dominating (the toy-dim GPU-time loss is undiscriminated until then).
- **`BenchResolver` re-key for fused GemmRelu** (29z-1) → a fuse=true CUDA bench (the cache keys by op, one op → one stage
  under fuse=false).
- **the non-CUDA graph APIs** (29-0, carried forward unchanged): D3D Compute Graph Compiler / MLIR Programs (Agility SDK
  public-preview/GA on the 4070; oracle-gated, semantic-engine) · VK_ARM_data_graph (an ARM Mali device) · NPU (an NPU device,
  §70 research) · D3D12 Work Graphs (a node-shader consumer) · CUDA device-graph launch (a graph needing device-side launch) ·
  Metal ICB (an Apple box) · cross-API interop transfers (a partition spanning two API memory domains) · ~~§103 multi-device
  sharding (a >1-GPU box)~~ **[LANDED at CEIR-30 — the §103 sharding MACHINERY (dist dialect + propagate/materialize + the Host+Gpu §140 proof) is DONE; only the >1-GPU HARDWARE remains deferred, not the machinery]** · autotuned provider choice (two providers both claim + cost decides — 28's tune cache is the home).

**DISSOLVED (not deferred):** the `IExecutionProvider` compile→plan pure-virtual APPEND from the 29-0 seam decision — reconciled
away at 29b-2b (`IExecutionProvider` is the scalar reference seam; a launch-graph provider is a plan launch-mode, not a
compile→plan implementor; the tensor "compile→plan" IS `plan_tensor_pipeline`). No such virtual was ever added; struck at 29-0.

## Scars (2 new memory files)

- `feedback_aliased_storage_graph_replay_must_reestablish_inputs` — a captured graph whose run overwrites an aliased slot that
  held its own input (a1 into x') cannot be replayed by relaunching alone; a faithful launch-many re-runs the prefix that
  re-establishes the input. READ the plan's alias table (don't guess).
- `feedback_bench_arms_must_match_wait_and_bracket_structure` — A/B GPU benches whose arms differ in submit/wait or event-bracket
  count measure the sync structure, not the work (the sandwich "loss" was 3 waits vs 1; the `enqueue` split fixed it). Includes
  the caching-resolver-for-repeated-execute clause. MEMORY.md pointers added for both.

## Uncommitted batch (the user commits — NO AI co-author trailer)

CEIR-29 rides the ongoing CEIR-26→29 working-tree batch (last commit `c35548a "working on CEIR-25"`). The CEIR-29 additions/mods
(see each tracker row's UNCOMMITTED-BATCH line for the authoritative per-slice list): `engine/ceir/include/crd/ceir/semantics.hpp`
(ProviderClass + provider_class_name/_from_name), `engine/ceir/ops/transform.ceirop.toml` + `engine/ceir/src/transform.cpp` +
`engine/ceir/generated/crd/ceir/gen/transform_ops.*` (assign_provider + constrain_provider_class directives),
`tools/ceir_opgen/ceir_opgen.py` (the `_cxx_ident` keyword sanitizer), `engine/ceir-gpu/{include,src}/…/partition_ml.*` +
`tensor_pipeline.*` + `expand_ml.*` (the partitioner + PlanStage.provider + lineage), `engine/gpu-context-cuda/…/cuda_compute_context.*`
(CUDA-Graphs capture + the `enqueue` split), `assets/ceir/{assign_provider_coopvec,constrain_provider_class_gpu}.ceir`, the CUDA +
band24 gates, `tests/ceir-gpu-cuda/CMakeLists.txt` (`_CRT_SECURE_NO_WARNINGS` for the bench env gate), and the bench board
`docs/bench/2026-09-05-ceir29-cuda-graphs-launch.md` + its `docs/bench/README.md` index line. `git status` is authoritative for
the complete list; the per-slice tracker rows carry the definitive per-slice files.

## Proposed commit message (Conventional Commits — NO AI co-author trailer, per CLAUDE.md/AGENTS.md)

```
feat(ceir): CEIR-29 native graph providers — §70 partitioner across provider classes

Device-free partitioner core (ProviderClass descriptor, maximal-subgraph greedy-grow,
authored provider pin + class-constraint directives) + the runnable CUDA-Graphs
launch-graph provider (capture/instantiate-once/launch-many, bit-exact vs the CKIR
fallback on Win RTX 13.3 + WSL2 RTX 12.0), the two-class boundary captured on-device,
and a launch-vs-N-dispatch bench. Adds CudaComputeContext::enqueue (single-submit
two-class launch) and an opgen C++-keyword sanitizer.
```
