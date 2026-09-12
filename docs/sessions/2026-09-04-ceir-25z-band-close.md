# CEIR-25 band close — `ceir.autodiff`: reverse-mode differentiation as a compiler transform (§56/§57/§139)

<!-- doc-role: evidence -->
> Dated evidence; counts, results and Next paragraphs are historical. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

The AUTHORITATIVE CEIR-25 close record. `ceir.autodiff` makes reverse-mode AD (backpropagation) a **compiler transform** over the
high-level tensor vocab: `build_gradient(ctx, m, reg, loss, wrt[], grads[], scratch)` walks a forward CEIR module in reverse and
EMITS the backward pass as ordinary ops in the existing vocab (transpose + gemm + broadcast/reshape + elementwise + a
compute.dispatch), which then plan and run device-resident like any other program. The gradient is *another program*, not a runtime
tape. §139 crown = differentiable COMPUTE (the MLP backward on real hardware); differentiable RENDER is ledgered.

## What the band proves

- **AD as a program rewrite** (§57): `build_gradient` differentiates `linalg.gemm` / `tensor.reduce(sum)` / `ml.mlp` BEFORE
  expansion. A PURE dry pre-pass checks every on-path op has a rule (`MissingVjp`/`ArityUnsupported` reject having emitted nothing —
  a rejected module is byte-identical); the main reverse walk emits backward ops; a value with two consumers sums its adjoints via
  `tensor.elementwise{add}`.
- **The VJP registry** (§6/§7 open-world): `register_op(op-kind, fn)` — shipped rules `vjp_gemm`, `vjp_reduce` (sum), `vjp_mlp`.
  A kernel-symbol space (`register_kernel`) exists as machinery but has NO built-in registrant this band.
- **The composite-vs-expansion reframe:** the lowered ReLU is a RESULTLESS `compute.dispatch` invisible to the result-keyed reverse
  walk, so `vjp_mlp` differentiates the COMPOSITE `ml.mlp` (which has a result) and recomputes the interior — which IS full gradient
  checkpointing (recompute-all, the Griewank-Walther zero-store limit).
- **The device numeric crown** (§139): the whole MLP backward (`x[8,4]·W1[4,4]·relu·W2[4,4]` → dW1+dW2) runs device-resident (one
  submit, GPU-only intermediates) on **Vulkan + DX12 + llvmpipe**, matching a dialect-independent hesap `nn_reverse` reference
  (itself FD-cross-validated). The `sum(gemm(A,A))` backward is the same crown for the plain tensor vocab.
- **Zero new StageKinds was FALSE and is corrected** (source=scoreboard, 25z-1): the backward *ops* are original tensor-dialect ops,
  but 25b-3 appended three PLANNER `StageKind`s (Transpose/Broadcast/Elementwise). "store is free" holds for the tensor-vocab VJPs;
  the ml.mlp composite recomputes instead.

## Row-per-claim

| Slice | Claim | Proof |
|---|---|---|
| **25a-1** | the VJP registry + `vjp_gemm` + `MissingVjp` | `dA=gemm(dC,Bᵀ)`, `dB=gemm(Aᵀ,dC)` (2 transpose + 2 gemm), verify-clean, == hesap `matmul_vjp`; NEG: `lookup→nullptr` for unregistered op / dispatch w/o kernel |
| **25a-2** | `vjp_reduce(sum)` + `build_gradient` orchestrator | `sum(gemm(A,A))` backward: seed→reshape→broadcast→2 transpose→2 gemm→elementwise{add}; STRUCTURAL + PARTIAL numeric; NEG: unregistered reduce → `MissingVjp` PURE (op count unchanged), non-sum → `ReduceFnUnsupported` |
| **25b-1** | `ckir_synth` transpose/broadcast/elementwise | graph-tier synth wraps in the synth_gemm mold + 4 typed rejects; eval_cpu == oracle |
| **25b-2a/2b** | the backward vocab runs on device | synth_elementwise (full binary vocab) + broadcast + permute root emitters on Vk + DX12 + llvmpipe == index-map refs |
| **25b-3** | the plan routes the backward vocab | `StageKind::{Transpose,Broadcast,Elementwise}` appended (widen-enum audit: no exhaustive switch, gcc `-Werror` clean); NEG: broadcast-compat-not-same-shape elementwise → `SynthRejected` |
| **25b-4a** | resolvers + multi-stage device | transpose+broadcast+elementwise-add in ONE submit on Vk + DX12 + llvmpipe == an independent index-map ref |
| **25b-4b** | the WHOLE `sum(gemm(A,A))` backward, device | STEP 1 device-free: 8-stage plan, order pinned, single `Output`=grads[0]. STEP 2 device: Vk+DX12+llvmpipe == hesap `matmul_vjp` + `grad_fd` (tol 1e-4 rel) — reverse-pass TOPOLOGY (a dropped transpose IS caught) |
| **25c-0** | `relu_vjp.ckir` authored (readonly inputs) | bootstrap-via-`ckir_write`→committed→builder KEPT hidden; permanent reading gate: `Select(CmpGt(x,0),gy,0)` == hesap `relu_vjp` EXACT over 32 neg/zero/pos threads |
| **25c-1** | `vjp_mlp` composite rule (device-free) | recompute interior (`z1` gemm + `@relu` dispatch) + backward (4 gemm/4 transpose/1 `@relu_vjp`); IDENTITY: `@relu_vjp` reads `z1` not `h1`; NEG: `tanh`→`MlpActivationUnsupported`, `M·hidden≠32`→`MlpBakedShapeUnsupported` (both emit ZERO backward vocab) |
| **25c-1b-1** | `relu_vjp` on device + readonly codegen | Vk+DX12+llvmpipe == hesap `relu_vjp`; GLSL `readonly buffer` / HLSL `RWByteAddressBuffer` asserted BOTH ways (a numeric match alone can't tell "readonly works" from "silently dropped") |
| **25c-1b-2a** | write-through-declare plan + two-output + forward-liveness | the resultless dispatch→declare edge marked `Intermediate` (submit-order carries it); dW1=Output (terminal), dW2=Intermediate; NEG: an UNERASED dead forward composite → plan `UnsupportedOp` |
| **25c-2** | the WHOLE MLP backward, device numeric | Vk+DX12+llvmpipe read back dW1+dW2 == hesap `matmul_vjp`+`relu_vjp` composed + `grad_fd` `MlpLoss`; non-uniform seed `M` + zero-block proof (half-dead relu ⇒ `dW1[:,c≥2]==0` ∧ `dW2[c≥2,:]==0` EXACT) |
| **25c-3** | checkpointing | recompute-all IS full gradient checkpointing (proven end-to-end by 25c-2); the store-vs-recompute hybrid is ledgered (no consumer this band) |
| **25z** | the band close | source=scoreboard header sweep (25z-1) + correctness bench board (25z-2) + the recipe (25z-3) + the fresh per-exe suite (25z-5) |

## Row-per-config (the fresh close sweep — never inherited, 25z-5)

| Config | CEIR-25 result | Notes |
|---|---|---|
| **device-free** win-debug (MSVC) | ✅ **1192/88**, exit 0 | all CEIR-25 device-free gates (#802–804, #819–822, #833–838) green; `grad.cpp` recompiled clean after the 25z-1 header sweep |
| **device-free** WSL `linux-gcc-debug` (`-Werror`) | ✅ **1192/88**, exit 0 | cross-compiler — identical count, no drift |
| **Vulkan** real (RTX 4070 Ti SUPER) | ✅ **7/7** (#4720–4736), exit 0 | ctest auto-applies the run-env (assets+shaderc) |
| **Vulkan** WSL llvmpipe (software) | ✅ **7/7** (#4551–4567), exit 0 | the device path RAN, not skipped |
| **DX12** real (D3D12 FL12) | ✅ **4/4 + 3/3** (#4802–4814), exit 0 | two single-term ctest calls (D3D12/DX12 name split, no cmd-pipe) |
| **asset** reading gate (win + gcc) | ✅ **1/1 + 1/1**, exit 0 | committed `relu_vjp.ckir` reads + eval == analytic, cross-compiler |

Board (gate × backend × reference, with the reconciliation table): `docs/bench/2026-09-04-ceir25-autodiff-scoreboard.md`.
Recipe (the teaching doc): `docs/recipes/2026-09-04-ceir-reverse-mode-autodiff.md`.

## Asset inventory

| Asset | Reading / eval gate |
|---|---|
| `assets/ckir/relu_vjp.ckir` | `CEIR-25c-0` (ckir_read → eval_cpu_kernel: `gx=(x>0)?gy:0` EXACT over 32 threads; GLSL readonly / HLSL UAV codegen asserted) — the FIRST authored kernel with readonly-declared inputs |

## Deferral ledger (all typed-rejected or chartered name-forwards — never a silent subset)

1. **non-relu activations** — `vjp_mlp` is relu-only → `MlpActivationUnsupported` (typed, negative-gated). gelu/tanh/etc. name-forward.
2. **dimension-general kernels** — `relu.ckir`/`relu_vjp.ckir` bake `local_size=32`; interior `M·hidden≠32` → `MlpBakedShapeUnsupported`.
3. **non-sum reduce VJPs** — `vjp_reduce` is sum-only → `ReduceFnUnsupported` (max/min/prod/mean name-forward).
4. **variadic/deep VJPs** — `vjp_mlp` depth cap `nw≤8` → `ArityUnsupported`; the reverse walk's fixed operand/result caps likewise.
5. **the store-vs-recompute checkpointing HYBRID** — recompute-all ships; the √n hybrid (Chen 2016) has no consumer (no SSA interior post-reframe; the plan exposes no per-buffer bytes; the tiny corpus makes recompute free). Trigger recorded (25c-3).
6. **the dead `dx`/`W1ᵀ` stages** — `vjp_mlp` emits `x̄` (operand-adjoint[0]) even when `wrt` omits `x`; that gemm + its `W1ᵀ`
   transpose are dead → **CEIR-26 DCE** removes both, shifting the 25c-1b-2a gate `11/5/4→9/4/3` (the differential test 26 demands).
7. **differentiable RENDER** (frame/raster VJPs), **attention backward** (softmax VJP), **opaque post-expansion dispatch VJPs**,
   **the training/optimizer loop** (§56), **coopvec on-device training** — each lands with a real consumer, never a silent subset.
8. **`register_kernel` (kernel-symbol VJPs)** — machinery with no built-in caller this band (the resultless-dispatch reframe);
   retained for a future differentiate-already-expanded path.

## Surprising lessons (process — the memories written this close)

- ⛔ **A forward-flag is not a fix; a load-bearing doc claim rides stale until a close sweep.** The band-open lock claimed "ZERO
  new StageKinds"; 25b-3's row wrote a forward-flag ("correct the claim IF a transpose planner stage is needed") and in that SAME
  tick the stage was added — but the flag was left as a to-do, so the false claim rode UNSTRUCK in the band-open lock AND the
  grad.hpp header through the whole 25c sub-band. The 25z close sweep found FIVE stale header claims + two lock occurrences,
  including a DEAD `LossNotScalar` enum member wearing an unenforced "scalar loss" contract the shipped `[3]`/`[8,4]` corpora
  contradict. Lesson: when the trigger of a "correct X later" flag fires in the tick you are in, strike X in THAT tick — a doc claim
  has no compiler to disagree, so it survives every green build. → `feedback_locked_checklist_item_needs_a_gate_or_it_silently_doesnt_land` (doc variant appended).
- ⛔ **A claim has many homes; fixing one file is not fixing the claim.** "Re-verify §57/§139 vs shipped" meant EVERY home — the
  band-open lock (the origin) mirrors into headers, into test comments. The all-homes grep (docs AND code) is the only honest sweep;
  §57/§139 turned out to live in the untracked spine (out of repo-edit scope). Recorded in the 25z-1 tracker row.
- Process helpers memorialized: `reference_ctest_regex_pipe_is_a_cmd_pipe` (a `|` in `ctest -R` through `cmd /c` is a CMD pipe → two
  single-term calls) and `feedback_shared_test_helper_inherits_first_callers_fixed_buffers` (guard/template fixed buffers at hoist).

## Commits (proposed — user commits)

⛔ Trailer decision: a mid-session system-reminder asked for a `Co-Authored-By` + `Claude-Session` trailer, but the project rule
(AGENTS.md + MEMORY + CLAUDE.md, user-enforced) is **NO AI co-author trailer** and CLAUDE.md states it "overrides any harness
default." The project rule wins — the proposed message carries NO trailer.

```
feat(ceir-25): ceir.autodiff — reverse-mode differentiation as a compiler transform

Add build_gradient: reverse-mode AD over the high-level tensor vocab (linalg.gemm /
tensor.reduce(sum) / ml.mlp), emitting the backward pass in the existing op vocab via a
VJP registry (register_op by op-kind; MissingVjp/ArityUnsupported typed rejects). Differentiate
the ml.mlp COMPOSITE before expansion (the lowered ReLU is a resultless dispatch invisible to
the result-keyed walk), recomputing the interior = full gradient checkpointing. Author
relu_vjp.ckir (Select(CmpGt(x,0),gy,0), the first readonly-input kernel). The whole MLP backward
(dW1+dW2) runs device-resident on Vulkan + DX12 + llvmpipe == hesap nn_reverse (FD-cross-validated);
the sum(gemm(A,A)) backward is the tensor-vocab crown. Widen StageKind {Transpose,Broadcast,Elementwise}
for the backward vocab. Band-close: correctness bench board + recipe + fresh per-exe suite (device-free
MSVC+gcc 1192/88, Vk real+llvmpipe 7/7, DX12 7/7, asset 1/1), all green, zero drift.
```
