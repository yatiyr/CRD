# Recipe — Reverse-mode autodiff as a CEIR compiler transform

<!-- doc-role: reference -->
> Technical reference; verify dated claims against current contracts/source. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

> How to make a whole class of GPU programs differentiable *at the IR level*: `build_gradient` walks a CEIR module in reverse
> and EMITS the backward pass as ordinary ops in the existing tensor vocab (transpose + gemm + broadcast/reshape + elementwise + a compute.dispatch),
> which then plan and run device-resident on Vulkan/DX12/llvmpipe like any other program. No tape, no eager engine — the gradient
> is *another program*, produced by rewriting the forward one. Code: `engine/execution/ceir-gpu/{include/crd/ceir/gpu,src}/grad.{hpp,cpp}`;
> board: [`docs/bench/2026-09-04-ceir25-autodiff-scoreboard.md`](../bench/2026-09-04-ceir25-autodiff-scoreboard.md).

## Parameters

| dial | value / type | meaning |
|---|---|---|
| `build_gradient(ctx, m, reg, loss, wrt[], grads[], scratch)` | entry point | reverse-mode over the backward-reachability set of `loss` wrt each `wrt`; APPENDS backward ops to `m`, fills `grads` (parallel to `wrt`; null ⇒ that wrt doesn't reach loss) |
| `loss` | a Value of **ANY rank** (NOT required scalar) | the output being differentiated; the seed carries ∂L/∂loss (see below), so a `[3]` or `[8,4]` loss is legal |
| `seed` (`GradResult.seed`) | ExternalIn the caller uploads **∂L/∂loss** into | ALL-ONES ⇔ implicit objective `L=sum(loss)`; a WEIGHTED tensor `M` ⇔ `L=⟨M,loss⟩` (non-uniform seed = the discriminating corpus). "=1" holds only at rank-0 |
| `wrt[]` | ConstSpan of Values | the inputs whose gradient you want (e.g. `{W1, W2}`) |
| VJP registry rules | `register_op(op-kind, fn)` | shipped: `linalg.gemm`→`vjp_gemm`, `tensor.reduce`→`vjp_reduce` (sum only), `ml.mlp`→`vjp_mlp` |
| kernel-symbol rules | `register_kernel(sym, fn)` | MACHINERY only — no built-in registrant this band (see the reframe trap) |
| `vjp_mlp` depth cap | `nw ≤ 8` (`h[9]`, `z[9]` scratch) | max layers; `nw > 8` ⇒ `ArityUnsupported` (typed reject, grad.cpp:390) |
| `vjp_mlp` activation | `relu` only | any other ⇒ `MlpActivationUnsupported` (typed reject) |
| baked-shape guard | ~~interior relu'd layer `M·hidden == 32`~~ **RETIRED 26d-4** | was `MlpBakedShapeUnsupported`; now `relu.ckir`/`relu_vjp.ckir` ship the shape SENTINEL (`local_size=0`) + the resolver cook-binds it, so a vjp runs at ANY interior width (proven h1=64 both backends). `MlpBakedShapeUnsupported` is DEAD-marked + KEPT (append-only enum) |
| `relu_vjp.ckir` | `local_size=0` (the 26d-4 SENTINEL — resolver cook-binds to the write numel); inputs `x@0`,`gy@1` **readonly**; `gx@2` writable | `gx = Select(CmpGt(x,0), gy, 0)` — EXACT strict `x>0` |
| device tolerance | f32 device vs f64 ref: **1e-4 rel**; authored `relu_vjp`: **EXACT** | 1e-4 derived per corpus (small f32 dot chains); pure-select kernel is bit-exact |
| device `Resolver::pipes[N]` (test harness) | **16** (was 8) | the test resolver's pipeline cap — the 11-stage MLP backward OVERRAN `[8]` (a scar, not a guessable default; bump in BOTH device TUs) |
| FD witness `MlpLoss::kCap` (test harness) | **64** | shared-header FD scratch cap; a corpus with `m·d1` or `m·d2 > 64` returns a deliberately-wrong `0.0` (fails the gate LOUD, never a silent stack overflow) |

## What it is / why it exists

Reverse-mode automatic differentiation (backpropagation) computes ∂L/∂(inputs) for a scalar objective in one backward sweep,
cost ~constant× the forward. The usual implementations are a **runtime tape** (record every op as it executes, replay in
reverse — PyTorch autograd) or an **eager tracer**. CEIR takes the third road, the one a *compiler* can take: differentiate the
program **as source**. `build_gradient` reads the forward module and writes a new sequence of ops that computes the gradient —
the backward pass is a first-class CEIR program, so it inherits everything the forward path has: the verifier, the planner, the
device executors, the asset/inspection tooling. A gradient is not a special runtime mode; it is *more IR*.

The naïve version of this fails for one specific reason, and understanding it is the whole design: **you cannot differentiate the
already-lowered program.** After `expand_ml`, an MLP's ReLU is a *resultless* `compute.dispatch(@relu, {h, relu_out})` — it writes
a buffer, it has no SSA result. `build_gradient`'s reverse walk keys on `op->result()` (an op with zero results is never visited),
so a lowered ReLU is invisible to the def-use walk and its gradient silently vanishes. The fix (the "reframe") is to differentiate
the **high-level composite** `ml.mlp` (which *does* have a result) BEFORE expansion, with a rule that reconstructs the interior and
emits the backward chain explicitly. Differentiate high, then lower — never differentiate low.

## The physics / maths

Reverse mode is the **vector-Jacobian product (VJP)**. For an op `y = f(x)` with output adjoint `ȳ = ∂L/∂y`, the VJP rule returns
the operand adjoint `x̄ = ∂L/∂x = Jᵀ·ȳ` (Jᵀ = the transpose of f's Jacobian, applied to ȳ). The whole backward pass is: seed the
output adjoint (`∂L/∂loss`), then visit ops in reverse topological order, each contributing to its operands' adjoints; a value with
two consumers **sums** its two partial adjoints (Baydin et al. 2018, *Automatic Differentiation in Machine Learning: a Survey*).

The three shipped rules:

- **gemm** `C = A·B` (A[M,K], B[K,N]): `Ā = C̄·Bᵀ` [M,K], `B̄ = Aᵀ·C̄` [K,N]. The standard matrix-calculus identity — emitted as two
  `tensor.transpose` (perm [1,0]) + two `linalg.gemm`. The `β·C` operand adjoint is null (not differentiated this band).
- **reduce(sum)** `out = Σ_axis input`: `∂out/∂input = 1`, so `īnput = broadcast(reshape(ōut, keepdim))` back along the reduced axis.
  The reshape re-inserts the size-1 at `axis` to dodge the 1-D right-align broadcast scar. Only `sum` (max/min/prod/mean are
  name-forward — a typed `ReduceFnUnsupported`).
- **relu** `y = max(x,0)`: `x̄ = (x>0) ? ȳ : 0`. Strict `>0` (matches the analytic subgradient choice at the kink); authored as
  `Select(CmpGt(x,0), ȳ, 0)` so `x==0 → 0` exactly, where a naïve `step()` would give `0.5` or `1`.
- **MLP composite** (2-layer): forward `z1=x·W1; h1=relu(z1); z2=h1·W2`. Backward from `ōut=z̄2`: `W̄2=h1ᵀ·z̄2`, `h̄1=z̄2·W2ᵀ`,
  `z̄1=relu_vjp(z1,h̄1)`, `W̄1=xᵀ·z̄1`, `x̄=z̄1·W1ᵀ` — pure chain rule. (`x̄` and its `W1ᵀ` transpose are emitted even when `wrt`
  omits `x`, so they are DEAD stages when you only want `dW1/dW2` — the board's `n_gemm==5` counts them. **Pruned by CEIR-26a DCE**
  (landed 2026-09-04): pin `grads[]` via `func.return` first — they are readback-by-Value, not SSA-consumed — then DCE removes the
  dead branch, `11→9` plan stages / **3 ops** (the `x̄` gemm + its output `resource.declare` + the `W1ᵀ` transpose). See CEIR-26a.)
  `vjp_mlp` recomputes `z1`,`h1` (they're not SSA on the
  composite) — this recompute-EVERYTHING IS **gradient checkpointing in its zero-store limit** (Griewank & Walther 2008,
  *Evaluating Derivatives* ch. 12 — the degenerate `revolve` with no stored checkpoints): full forward recompute, zero stored
  activations. Chen et al. 2016 (*Training Deep Nets with Sublinear Memory Cost*, arXiv 1604.06174 §3) is the √n store-vs-recompute
  HYBRID — that is the variant ledgered (CEIR-25c-3) with a precise trigger and no consumer yet; here the corpus is tiny so
  recompute-all is free.

## The full assembly

1. **`build_gradient`** (`grad.cpp`): mark the backward-reachability set of `loss` wrt `wrt`; create the `seed` declare
   (∂L/∂loss); run a **PURE dry pre-pass** that checks *every* on-path op has a registered rule and fits the reverse-walk arity —
   `MissingVjp`/`ArityUnsupported` reject here having emitted NOTHING (a rejected module is byte-identical). Then the main reverse
   walk emits backward ops through the registry; a value with two consumers gets `tensor.elementwise{fn=add}` to sum adjoints.
2. **The VJP registry** (`VjpRegistry`): open-world dispatch (§6/§7 — never `switch(op.kind)`). Two maps: op-kind rules and
   kernel-symbol rules. `lookup` resolves `compute.dispatch` by its `kernel` symbol and everything else by kind; nullptr ⇒
   `MissingVjp` (unregistered ≠ zero gradient).
3. **`vjp_mlp`** (`grad.cpp`): differentiates the composite before expansion — emits the interior recompute (`z1` gemm, `h1` as a
   `compute.dispatch(@relu)`) + the backward chain (gemms, transposes, `compute.dispatch(@relu_vjp)`). `x̄` (operand adjoint [0]) is
   emitted LAST so `grads[wrt=W1]` is the terminal op = the plan's single readback target.
4. **`plan_tensor_pipeline`** (`tensor_pipeline.cpp`): routes each backward op to a `StageKind` — Gemm / Transpose / Broadcast /
   Elementwise / VizDispatch — and assigns each buffer a role (ExternalIn / Intermediate / Output / Alias). The resultless
   `@relu`/`@relu_vjp` dispatches WRITE a `resource.declare` with no SSA edge; the plan still sees the write and marks the buffer
   `Intermediate`; correctness of the dispatch→later-reader edge rides submit order + the executor's post-stage barrier.
5. **Execution** (`execute_tensor_pipeline` + the device test harness): seed ExternalIn buffers by SSA-value identity, read back the
   `Output` (and any named Intermediate) by-Value. One submit; intermediates are GPU-only.

The **oracle** side (never CEIR): `crd::hesap::autodiff::reverse::nn` composes `matmul_vjp`+`relu_vjp` in f64 for the analytic
reference, itself FD-cross-validated by `gradient_check::grad_fd` before it judges the device. A shared indexing bug can't pass
both hesap and CEIR because they are separate codebases.

## The traps

- **The reframe — differentiate the composite, not the expansion.** `expand_ml` emits ReLU as a RESULTLESS `compute.dispatch`;
  `build_gradient` keys on `op->result()`, so a resultless op is never visited and its gradient silently disappears. Symptom: a
  lowered MLP "differentiates" but the backward pass has no ReLU derivative. Fix: register a rule on the **composite** `ml.mlp`
  (which has a result) and recompute the interior inside the rule. Corollary: `register_kernel` (the kernel-symbol VJP space) has
  **no built-in caller** this band — it's retained machinery, not a live path.
- **Write-through-declare.** A resultless dispatch writes a `resource.declare` — there is NO SSA edge from the dispatch to the
  buffer, and none from the buffer to its later reader. If you assume "every buffer a stage reads has an SSA writer," you miss this
  entirely. The plan marks such a buffer `Intermediate` (it *does* see resultless writes), and the read is correct only because the
  stages submit in order with a barrier between. Gate it by asserting the buffer's role AND that its first reader's stage index is
  strictly after the dispatch's.
- **Two-output readback.** In `{W1,W2}` gradients, `dW1` is the terminal op = the single `Output`; `dW2` is emitted earlier, unread
  by the graph, so it's `Intermediate` = GpuOnly. On **Vulkan** a GpuOnly buffer is unmappable (`map()`→nullptr), so you must force
  its memory class to `GpuToCpu` at create time to read it back. On **DX12** the portable pattern already stages every readback
  through a dedicated `GpuToCpu` copy, so an Intermediate reads back exactly like an Output — no role-forcing. Record all barriers
  and copies BEFORE the single submit (a barrier after submit is a no-op → the second readback races).
- **Forward-liveness is the caller's decision.** `build_gradient` only APPENDS the backward; it does not remove the forward op it
  differentiated (a training loop wants both loss and grads). A caller planning the backward ALONE must ERASE the now-dead forward
  composite first (the planner typed-rejects a non-plannable `ml.mlp` as `UnsupportedOp`). Erase is safe iff the result has no
  users (`assert !loss->has_uses()`). Engine-side DCE of a dead differentiated op is ledgered CEIR-26.
- **Readonly inputs codegen differently per backend, and a numeric match can't catch it.** `relu_vjp.ckir` declares `x`/`gy`
  readonly. GLSL emits `readonly buffer` SSBOs; **HLSL drops readonly and emits all `RWByteAddressBuffer` UAVs**. A device-numeric
  match alone cannot distinguish "readonly works" from "readonly silently dropped," so assert the SOURCE both ways (identity, not
  category): GLSL *has* `readonly buffer`, HLSL *has no* `readonly` and *is* `RWByteAddressBuffer`.
- **~~Baked `local_size=32`~~ → COOK-BIND SENTINEL (RETIRED 26d-4).** `relu.ckir`/`relu_vjp.ckir` originally baked one workgroup of
  32 with no bound guard, so `vjp_mlp` rejected an interior `M·hidden ≠ 32` (`MlpBakedShapeUnsupported`, the cook-only-ships-device-
  impossible scar). CEIR-26d shape-specialization RETIRED that: both assets now ship the SENTINEL `local_size=0` and the VizDispatch
  resolver cook-binds `local_size` to the intermediate's write numel (`bind_authored_local_size`), so a vjp of an MLP at ANY interior
  width ≤ the single-workgroup cap runs device-resident (proven h1=64 on Vk+DX12, 26d-4b/4c). The reject is gone; the guard's teeth
  moved to the resolver's `LocalSizeExceedsLimit` (numel > cap). `MlpBakedShapeUnsupported` is DEAD-marked + KEPT (append-only enum).
- **Finite differences are invalid at the ReLU kink.** A central-difference witness on `f(W)=loss(mlp(x,W))` corrupts wherever a
  pre-activation `z1 ≈ 0`. Use a **column-signed off-kink corpus** (x all-positive, W1 columns split +/−) so `z1` columns are
  cleanly ± and well-separated (`|z1|>0.05`); this also gives a **zero-block proof** — half the hidden units dead ⇒ `dW1[:,dead]==0`
  and `dW2[dead,:]==0` EXACT, which a dropped/wrong-half `relu_vjp` breaks.
- **Do NOT add a scalar-loss check.** `loss` is any-rank — the seed carries `∂L/∂loss`, so a `[3]` or `[8,4]` loss differentiates
  fine. `GradError::LossNotScalar` exists but is a DEAD member (the "loss must be rank-0" contract was never enforced and is wrong);
  a rebuilder who "helpfully" enforces it breaks the shipped corpora. (Doc-hygiene aside: the band-open "ZERO new StageKinds" claim
  was also false — 25b-3 added three *planner* StageKinds though no new *ops*; both corrected at 25z-1, see the tracker.)

## Measured numbers

The full gate × backend × reference board: [`docs/bench/2026-09-04-ceir25-autodiff-scoreboard.md`](../bench/2026-09-04-ceir25-autodiff-scoreboard.md).
Headline: **every CEIR-25 gate green on every backend, no losses**; the whole MLP backward (dW1+dW2) runs device-resident on
**Vulkan + DX12 + llvmpipe** matching a dialect-independent hesap reference (FD-cross-validated), plus the `sum(gemm(A,A))`
tensor-vocab crown. Never quote these numbers from memory — read the board.

## Where the code lives

- `engine/execution/ceir-gpu/include/crd/ceir/gpu/grad.hpp` + `src/grad.cpp` — `build_gradient`, `VjpRegistry`, `vjp_gemm`/`vjp_reduce`/`vjp_mlp`, `register_builtin_vjps`, `GradError`.
- `engine/execution/ceir-gpu/include/crd/ceir/gpu/tensor_pipeline.hpp` + `src/tensor_pipeline.cpp` — `plan_tensor_pipeline`, `StageKind`, `execute_tensor_pipeline`.
- `engine/execution/ceir-gpu/src/ckir_synth.cpp` — `synth_transpose`/`synth_broadcast`/`synth_elementwise` (the backward vocab's kernel-tier synth).
- `assets/ckir/relu_vjp.ckir` — the authored ReLU-VJP compute kernel (first with readonly-declared inputs).
- Gates: `tests/execution/ceir-gpu/test_grad.cpp` (structural + typed rejects), `tests/execution/ceir-gpu/test_tensor_pipeline.cpp` (plan gates),
  `tests/execution/ceir-gpu-vulkan/test_ceir_pipeline_vulkan.cpp` + `tests/execution/ceir-gpu-dx12/test_ceir_pipeline_dx12.cpp` (device numeric),
  `tests/gpu/gpu-shared/autodiff_fd_functors.hpp` (`SumGemmAA`/`MlpLoss` FD witnesses), `tests/gpu/kir/test_ckir_asset.cpp` (the `relu_vjp.ckir` reading gate).
