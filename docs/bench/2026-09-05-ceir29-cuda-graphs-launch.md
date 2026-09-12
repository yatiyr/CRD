# 2026-09-05 — CEIR-29z: CUDA-Graphs launch vs the N-dispatch fallback

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The CEIR-29 band-close board (29z-1). **What is measured:** the runnable §70 native-graph provider (CUDA Graphs) launching a
captured tensor-pipeline run, vs the same pipeline run as N separate dispatches (the CEIR-29b-1 fallback). This is a
Cerid-INTERNAL A/B (our fallback vs our graph launch), NOT a peer crush — the peer is our own N-dispatch path.

⛔ **The win is the CPU submit path, not GPU time.** Both arms run the IDENTICAL kernels in the IDENTICAL stream order, so a
GPU-event bracket around each measures near-identical *kernel* work; the CUDA-Graphs win is the CPU cost of submitting it (N
`cuLaunchKernel` + the recorder walk vs one `cuGraphLaunch`). This is the same shape as the REN-1 frame-graph board
(`2026-07-24-ren1-frame-graph-batching.md`: N-submit vs 1-submit, CPU submit-path wall-clock). Two columns:
`cpu_us` = CPU wall-clock per pipeline iteration (`std::chrono::steady_clock`, median of 9, per-iteration = wall/K); `gpu_us`
= `CudaComputeContext::last_gpu_ms()` (one CUDA-event bracket per iteration).

## Machine / config

| | Windows | WSL2 |
|---|---|---|
| GPU | RTX 4070 Ti SUPER | RTX 4070 Ti SUPER (same box, GPU-passthrough) |
| CUDA | 13.3 (native driver) | 12.0 (passthrough driver) |
| Build | `build/win-debug` (MSVC 14.50) | `linux-gcc-debug` (gcc, `-Werror`) |
| Kernels | NVRTC → CUBIN, `--fmad=false` (bit-exact vs the CPU oracle) | same |

## Harness (re-runnable)

`tests/ceir-gpu-cuda/test_ceir_pipeline_cuda.cpp`, TEST_CASE `"ceir 29z"` (tag `[bench]`), env-gated on `CRD_CEIR_BENCH` so
the default ctest sweep does not time. `bench_captured()`: upload once; a CACHING resolver (`resolve_stage_cached`, keyed by
`st.op`) so repeated `execute_tensor_pipeline` calls reuse pipelines (the shared `resolve_stage` caps at 16 and never caches —
fine for the correctness runners' handful of calls, fatal for a bench's hundreds); warmup 5; median of 9; K ∈ {1, 10, 100}
launches per timed sample (amortizes nothing one-time — capture/instantiate are outside the loop — so K quantifies per-launch
CPU cost and shrinks timer noise). Run: `CRD_CEIR_BENCH=1 ctest -V -R "29z"` (or the exe with `-s` to see the rows).

⛔ **Both arms are ONE event bracket + ONE wait.** The fallback = `begin()` → `execute` the whole plan (N dispatches) →
`submit_and_wait()`. The graph = `begin()` → eager prefix dispatches → **`enqueue(graph)`** → eager suffix dispatches →
`submit_and_wait()` (the CEIR-29z `CudaComputeContext::enqueue` split — a non-waiting `cuGraphLaunch` on the recording stream,
so the two-class prefix + graph + suffix are ONE submission; same-stream order carries the x'→graph→z dependency). For the
whole-plan `mlp3` fixture the prefix/suffix are empty, so its graph arm is `begin/enqueue/submit_and_wait` (the graph alone).
⛔ Without the enqueue split the graph arm was three `submit_and_wait` round-trips (prefix + `launch` + suffix), which timed
wait-latency ×3 vs ×1 and made the sandwich a spurious 0.49–0.87× "loss" — a harness artifact, now removed.

## Fixtures

- **mlp3** — the 3-stage `gemm → relu → gemm` MLP (dims M=4, 8→8→2), the whole plan captured into ONE graph (`cap=[0,3)`,
  `node_count=3`). Graph arm = one `cuGraphLaunch` vs 3 `cuLaunchKernel`.
- **sandwich5** — the CEIR-29c-2 two-class plan `gemm(x,W0) → mlp_relu(·;W1,W2) → gemm(·,W3)` (5 stages), the middle 3-stage
  `cuda_graphs` run captured (`cap=[1,4)`, `node_count=3`) flanked by 2 eager fallback gemms. Graph arm = 2 eager dispatches +
  one `cuGraphLaunch` (in one submit) vs 5 `cuLaunchKernel`.

## CPU submit-path — `cpu_us` per pipeline iteration (median of 9); `x` = fallback / graph (higher = graph faster)

| device | fixture | K=1 fb | K=1 gr | K=1 x | K=10 fb | K=10 gr | K=10 x | K=100 fb | K=100 gr | K=100 x |
|---|---|---|---|---|---|---|---|---|---|---|
| Windows | mlp3 | 17.4 | 14.5 | **1.20×** | 24.8 | 14.6 | **1.70×** | 21.9 | 15.0 | **1.46×** |
| Windows | sandwich5 | 41.7 | 18.6 | **2.24×** | 36.6 | 26.7 | **1.37×** | 37.2 | 27.4 | **1.36×** |
| WSL2 | mlp3 | 29.7 | 26.5 | 1.12× | 28.2 | 25.9 | 1.09× | 28.6 | 31.3 | 0.92× |
| WSL2 | sandwich5 | 46.6 | 29.7 | **1.57×** | 36.1 | 31.9 | **1.13×** | 48.4 | 39.8 | **1.22×** |

## GPU time — `gpu_us` per iteration (one event bracket both arms); NOT ratioed at this scale

| device | fixture | fb gpu_us | gr gpu_us |
|---|---|---|---|
| Windows | mlp3 | 8.3 | 36.8 |
| Windows | sandwich5 | 11.3 | 44.3 |
| WSL2 | mlp3 | 7.2 | 31.3 |
| WSL2 | sandwich5 | 12.3 | 37.6 |

## Verdict

**The CUDA-Graphs submit-path win is real and consistent for the two-class pipeline** (`sandwich5`): graph faster on BOTH
devices at every K — Windows 1.36–2.24×, WSL2 1.13–1.57× — because the fallback pays 5 dispatch-submit CPU costs vs the graph
arm's 2 eager + 1 `cuGraphLaunch`. The whole-plan `mlp3` (3 tiny dispatches) is a smaller, noisier win: Windows 1.20–1.70×,
WSL2 ~parity (0.92–1.12×) — with only 2 launches saved the CPU delta is near WSL's passthrough-driver jitter floor. The win
GROWS with dispatch count (5-stage sandwich > 3-stage mlp), the expected shape.

⛔ **GPU-time LOSS at toy dims, stated honestly (SANITY #6):** on both devices the graph's `gpu_us` is ~3–4.5× the fallback's.
Both arms are ONE event bracket + one wait, so this is not the multi-bracket artifact the earlier draft had — but the ~25 µs
delta is UNDISCRIMINATED at this scale: it is either `cuGraphLaunch`'s fixed GPU-side launch overhead, or an ev0→first-graph-
node scheduling latency a directly-launched kernel does not pay. Telling those apart needs a per-bracket-floor control (a
1-node slice timed three times vs a 3-node bracket) which is not run here. Either way the delta is a fixed cost dominating the
~7–12 µs of actual toy-kernel work (M=4, 8-wide), so the graph reaches GPU parity only when the captured work amortizes it —
NAME-FORWARD to a larger-fixture pass with that floor control (out of scope for a band close). At these dims the graph is a
CPU-submit-path win with a GPU-launch fixed-cost tax; do NOT read the `gpu_us` ratio as `cuGraphLaunch` overhead alone.

## Falsifiability / honesty gates (ON during the timed run)

- **Bit-exact** — both arms' output compared to the CPU MLP oracle (`--fmad=false` + sequential-k); a timing loop that skipped
  the check is the `--gpu-cull` empty-canvas scar.
- **`node_count == 3`** both fixtures (the graph captured exactly the claimed run — 3 dispatches, NOT the sandwich's total 5).
- **`gpu_us > 0`** both arms (the 28b "median==0 ⇒ measuring nothing" hard-fail — the graph actually launched real work).
- Medians are single-run representatives; run-to-run jitter is visible (Windows `mlp3` K10 varied ~1.24–1.70× across runs,
  WSL2 more) — the sandwich CPU win and the GPU-overhead loss reproduce on every run and both devices.
