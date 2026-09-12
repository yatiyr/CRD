# 2026-09-05 — CEIR-30z: the sharded-reduction transfer-cost decomposition

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The CEIR-30 band-close board (30z-1). **What is measured:** the §140 sharded reduction (a `[rows,cols]` tensor sharded axis-0
into two `[rows/2,cols]` ranks → per-rank `reduce(axis0,sum)` → an all-reduce elementwise combine) run three ways on the SAME
lowered `[Reduce,Reduce,Elementwise]` plan, to quantify the cost of the cross-domain PLACEMENT decision. This is a
Cerid-INTERNAL decomposition (all-Host baseline vs the Host+CUDA split vs the pure transfer floor), NOT a peer crush.

⛔ **This is the census-predicted HONEST LOSS made quantitative (SANITY #6), stated at 30-0 so 30z is not surprised.** On
single-GPU hardware the sharded arm CANNOT win: the two per-rank reduces run on the CPU (the reduce cannot run on CUDA this band
— see the N/A row), so the split adds a fixed cross-domain tax (2 partial uploads + 1 readback) on top of the SAME Host reduces,
with no parallel-device benefit. The board's value is the *shape*: the tax is a FIXED per-crossing cost, amortized by shard
compute — `B/A` collapses from ~300× (Win) / ~1000× (WSL2) at `[8,4]` to **~1.05–1.12× at `[4096,512]`**.

## Machine / config

| | Windows | WSL2 |
|---|---|---|
| GPU | RTX 4070 Ti SUPER | RTX 4070 Ti SUPER (same box, GPU-passthrough) |
| CUDA | 13.3 (native driver) | 12.0 (passthrough driver) |
| Build | `build/win-debug` (MSVC 14.50) | `linux-gcc-debug` (gcc, `-Werror`) |

## Harness (re-runnable)

`tests/ceir-gpu-cuda/test_ceir_pipeline_cuda.cpp`, TEST_CASE `"ceir 30z"` (tag `[bench]`), env-gated on `CRD_CEIR_BENCH` so the
default ctest sweep does not time. `bench_one_dim()` builds the §140 chain at `[rows,cols]`, lowers it (`lower_sharded_reduction`
→ `[Reduce,Reduce,Elementwise]`, placement `[Host,Host,Gpu]` from `stage_class_from_partition`), sets up all buffers ONCE (no
alloc / no re-seed in the timed loop — the reduces read the shards read-only and regenerate the intermediates each run), then
times three arms. Warmup 5, median of 9, K=10 iterations per sample, per-iter = wall/K (`std::chrono::steady_clock`). Arm B uses
a CACHING gpu-stage resolver (`bench_gpu_stage`, keyed by `st.op` over `cuda_gpu_stage` — the 29z `resolve_stage_cached`
rationale: 90 executes would otherwise re-create the combine pipeline every call). Run:
`CRD_CEIR_BENCH=1 ctest -V -R "30z"` (or the exe with `-s`).

**Three arms, one plan:**
- **A = all-Host** — `execute_tensor_pipeline_host` on the whole lowered plan. Device-FREE, no transfers. The baseline.
- **B = sharded `[Host,Host,Gpu]`** — `execute_two_class`: the two reduces on the Host, the all-reduce combine on CUDA, with 2
  partial uploads (`HostToDevice`, the reduce outputs) + 1 readback (`DeviceToHost`, the combine result).
- **C = the transfer FLOOR** — arm B's 3 planned transfers via `cuda_transfer`, with NO compute (matching B's per-transfer
  submit-and-wait structure). The discriminator: is the tax the crossing, or the CUDA combine?

⛔ **`cpu_us` is the headline** (CPU wall-clock per pipeline iteration — what the caller pays for the placement). `combine_gpu_us`
is `CudaComputeContext::last_gpu_ms()` of the Gpu combine's OWN bracket, **snapshotted into a field inside `bench_gpu_stage`
immediately after the combine's `submit_and_wait`** — before the trailing readback submit runs. This is load-bearing: `last_gpu_ms`
tracks the *last* submit-and-wait, so the readback (a later `cuda_transfer` submit in the same execute) would overwrite it; the
board number is the value captured at the combine instant, never read back externally. It is the combine ONLY, NOT the whole arm.
Arm A has NO device bracket (device-free) — no `gpu_us` column for it.

## CPU wall-clock per iteration (median of 9, µs) — `A` = all-Host, `B` = sharded, `C` = transfer floor

### Windows (RTX 4070 Ti SUPER, native driver)

| dim `[rows,cols]` | A_host µs | B_sharded µs | C_xferfloor µs | tax (B−A) µs | combine_gpu µs | **B / A** |
|---|---|---|---|---|---|---|
| [8, 4]      | 5.45     | 1883.8   | 1703.1   | 1878.4  | 32.9  | **345.7×** |
| [64, 32]    | 26.65    | 1875.8   | 1673.0   | 1849.2  | 31.8  | **70.4×** |
| [512, 128]  | 649.9    | 2630.9   | 1643.3   | 1981.0  | 86.0  | **4.05×** |
| [4096, 512] | 28666    | 31380.6  | 1729.6   | 2714.6  | 89.7  | **1.095×** |

### WSL2 (RTX 4070 Ti SUPER, passthrough driver)

| dim `[rows,cols]` | A_host µs | B_sharded µs | C_xferfloor µs | tax (B−A) µs | combine_gpu µs | **B / A** |
|---|---|---|---|---|---|---|
| [8, 4]      | 3.63     | 3702.8   | 3331.6   | 3699.1  | 97.7  | **1020.8×** |
| [64, 32]    | 26.61    | 3889.9   | 3582.2   | 3863.2  | 97.7  | **146.2×** |
| [512, 128]  | 697.6    | 4659.8   | 3368.0   | 3962.2  | 104.9 | **6.68×** |
| [4096, 512] | 30424.9  | 34023.2  | 3131.6   | 3598.3  | 97.0  | **1.118×** |

## Verdict

**The cross-domain placement tax is a FIXED per-crossing cost, amortized by shard compute.** Two facts carry it, both on both
devices:

1. **`C_xferfloor` is ~constant across the whole sweep** (Win ~1640–1730 µs, WSL2 ~3130–3580 µs) — it does NOT grow with `rows`,
   because what crosses the domain is the two `[cols]` partials (the reduce OUTPUTS) + the `[cols]` result, never the
   `[rows/2,cols]` input shards (those are consumed on the Host). The floor is the 3 submit-and-wait round-trips; WSL2's
   passthrough driver roughly doubles the per-submit latency (~1.7 ms → ~3.3 ms), the only cross-device difference.
   *(VRAM note: the `run_two_class_cuda` device table allocates a device buffer for EVERY plan buffer — so the `[4096,512]` run
   shows ~8 MB device residency for the two `[2048,512]` shard buffers even though the shards are Host-consumed and never
   uploaded. Created-but-unwritten is the two-class setup pattern, not a bench leak; devalloc stayed at its 64 MB pool.)*
2. **`tax (B−A) ≈ C_xferfloor` at every dim**, and both DWARF `combine_gpu` (20–105 µs). So the tax is the CROSSING, not the
   combine — arm C is the discriminator the 29z board lacked (its GPU-overhead gap was left "undiscriminated"; here it is resolved).

`A_host` grows `O(rows×cols)` (5 → 27 → 650 → 28666 µs on Win), so as the shard reduce grows the fixed ~1.7 ms (Win) / ~3.3 ms
(WSL2) tax amortizes: **`B/A` 345× → 70× → 4.0× → 1.10× (Win)**, **1021× → 146× → 6.7× → 1.12× (WSL2)**. At `[4096,512]` the
sharded run is only ~5–12% slower than all-Host — the tax is nearly hidden by the 28–34 ms reduce.

⭐ **This IS §140 "placement is semantic, not glue":** the SAME lowered plan runs all-Host and `[Host,Host,Gpu]` **bit-exact** vs
the two-stage oracle (the falsifiability gate below), and the only difference is WHERE each stage ran — a free, authored choice
(CEIR-30c) with a measurable, honest cost. The crossover to a WIN needs the deferred hardware: a real second GPU (below) would
run the two per-rank reduces IN PARALLEL, turning the `A_host` term into `max` instead of `sum` while the tax stays fixed.

## N/A — stated with the check (SANITY #9)

- **No all-Gpu arm.** The per-rank compute is a `tensor.reduce` over the sharded (axis-0) dimension; `emit_reduce_cuda`
  (`ckir_cuda.hpp`) is trailing-axis-only and its 2-scalar push is incompatible with the single-blob CUDA dispatch — the
  reduce-on-CUDA contract is ledgered (CEIR-30 deferral, trigger: a CUDA shard whose per-rank compute is a reduce). So the
  single-device baseline is all-Host, not all-Gpu. **Check:** the plan's stage-0/1 are `StageKind::Reduce`; a CUDA resolve of a
  reduce stage returns `UnresolvedKernel` (asserted in the 30b-3b arm-guard comment).
- **No real 2-GPU arm.** Single RTX 4070 Ti SUPER + WSL2 passthrough of the same GPU — no second physical device (CEIR-30
  deferral, trigger: a >1-GPU box). The two `[Host,Host,·]` reduces are serialized on one CPU; a 2-GPU mesh is where the parallel
  win lives. **Check:** the census recorded the hardware as the go/no-go at 30-0.

## Falsifiability / honesty gates (ON during the timed run)

- **Bit-exact** — arm A and arm B outputs compared to the two-stage f32 oracle `fold(shard0 col c)+fold(shard1 col c)`, asserted
  after **10 back-to-back sharded executes** (not iteration 1) — the aliased-storage-replay clobber catch (a readback that
  clobbered a partial would drift by iteration 10).
- **`n_xfer == 3`** per execute (2 partial uploads + 1 readback — the planned crossings, `== plan_transfers(plan,sc).size()`).
- **`combine_gpu > 0`** (the 28b "median==0 ⇒ measuring nothing" hard-fail — the Gpu combine actually launched real work).
- Medians are single-run representatives; run-to-run jitter is visible (Win `B_sharded` at `[8,4]` was 1621 / 1676 / 1580 /
  1884 µs across the four captured Win runs, the tax ~1200–2715 µs) — the amortization shape (`B/A` → ~1.1× at `[4096,512]`) and
  `C ≈ tax ≫ combine` reproduce on every run and both devices.
