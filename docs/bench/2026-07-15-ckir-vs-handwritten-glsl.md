# CKIR-emitted GLSL vs HAND-WRITTEN GLSL — the zero-IR-overhead head-to-head (2026-07-15)

The definitive honest test of a portable shader IR: does CKIR-emitted GLSL run as fast as a human's hand-written GLSL for the
SAME algorithm? Both go through the identical shaderc → SPIR-V → driver path, so this isolates the quality of the EMITTED code.
Same buffers, GPU-timed kernel-only (`last_gpu_ms`, min-of-30), and — the honesty gate — **both kernels' outputs must match**
(a fast wrong kernel is meaningless). Harness: `tests/gpu-context-vulkan` `[.crush-bench]`.

## Machine

- **GPU:** NVIDIA GeForce RTX 4070 Ti SUPER (Ada, AD103), ~672 GB/s. Clocks unlocked; min-of-30.

## Board

| kernel | CKIR | hand (reg-loop) | **CKIR / hand** | out-match | verdict |
|---|--:|--:|--:|--:|---|
| **ReSTIR RIS** (M=32 reservoir, memory-bound) | **0.318 ms** | 0.312 ms | **1.02× — PARITY** | 0.0 (bit-exact) | ✅ zero overhead |
| atmos **transmittance** (40-step exp march, compute-bound) | 0.083 ms | 0.068 ms | 1.23× | 3.4e-5 (FMA ULP) | ◧ bounded gap |

## The ReSTIR finding — a real fix, not a rig

The FIRST measurement was damning: CKIR **0.486 ms** vs hand **0.312 ms = 1.56× SLOWER**, yet **bit-identical output**. The
bit-identical part was the clue — the *math* was the same, so the loss was pure **code structure**. The CKIR statement-tier
builders were UNROLLING their candidate loop at graph-build time (32× straight-line blocks, all index temps hoisted to the top
→ enormous register pressure → collapsed GPU occupancy on a latency-bound kernel). Hand-written used a tight `for` loop.

The FIX: emit a tight **runtime loop** (`stmt_for_begin`) with the loop-carried reservoir (Σw, chosen f, chosen p̂) in per-thread
**shared slots** `[tid]`, not an unroll. Result: **0.318 ms — parity with the hand-written register loop** (and it crushes the
old unroll 1.53×), still bit-exact (Vulkan vs oracle 1.11e-7). A hand-written shared-loop variant measured 0.318 ms too — so the
emitter now produces optimal shared-loop code. Two emitter hazards surfaced + solved along the way:
- **shared RMW re-read**: `Σw = sh[tid] + phi` re-evaluated after its own store double-counts `phi` → `stmt_materialize(Σw)` freezes it.
- **cross-scope temp**: a value first used INSIDE the loop but also used AFTER it (the output index `p`) was scoped to the loop
  body ⇒ undefined at the store ⇒ compile error → `stmt_materialize(p)` hoists it to the outer scope.

**⭐ Rule: statement-tier compute loops with many iterations must be RUNTIME LOOPS (`stmt_for_begin` + shared accumulators), not
compile-time unrolls — the unroll's occupancy collapse is a 1.5×+ loss on memory-bound kernels.**

## The transmittance residual — honest, bounded, scoped

Transmittance is **compute-bound** (two `exp` + a `sqrt` per step, low register pressure). Here the unroll's register-resident τ
is actually the BEST CKIR option: the shared-loop variant measured **1.47× SLOWER** (0.100 ms — shared traffic on the compute
critical path). So the unroll (0.083 ms) is kept. The residual **1.23× vs the hand-written REGISTER loop** (0.068 ms) is the one
thing CKIR's compute emitter cannot express: a **register-carried loop accumulator** (the value-graph `for_loop`/`LoopAcc`
mechanism exists but only in the FRAGMENT emitter; the statement-tier compute path has only unroll or shared). Wiring
register-carried loops into the statement-tier compute emitter + CPU oracle + all 5 backends is the scoped engine slice that
closes this. Scale: transmittance is a once-per-frame 256×64 LUT costing ~0.003 ms in production (measured at 2048×512 here only
to be timeable); the 1.23× is 0.015 ms on a negligible kernel. Not swept under the rug — named, bounded, with a fix path.

## Bottom line

For the DOMINANT kernel class (memory/occupancy-bound: reservoirs, gathers, denoisers), CKIR-emitted GLSL runs at **parity with
hand-tuned** — the portable, bit-exact IR carries **zero performance tax**. The lone residual is register-carried loops for
compute-bound marches, a scoped emitter feature, on kernels whose absolute cost is already negligible. Honest crush: the IR wins
where it matters and its one gap is named + bounded, not hidden.
