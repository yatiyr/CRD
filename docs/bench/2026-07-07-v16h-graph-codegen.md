# 2026-07-07 — v16-h: structural graph AD + tape→C++ codegen

**What shipped (`graph_ad.hpp`):** trace a scalar-generic functor into an expression DAG (`Graph` + `GExpr`
overloads), differentiate it **symbolically** (`reverse_ad` emits the gradient as NEW graph nodes — reverse-mode over
the DAG), run **const-fold → CSE (hash-cons) → DCE** (`optimize`), then either **interpret** (`eval`) or **emit a
straight-line C++ kernel** (`emit_cpp`) — the Enzyme/Tapenade source-transform lane, but as PORTABLE C++ (no LLVM-plugin
lock-in, no XLA runtime). A codegen'd kernel drops the tape's per-node dispatch + per-call rebuild ⇒ a compiled hot
loop; CSE/DCE shrink the op count.

## ★ Gate (`test_graph_ad.cpp`, win-debug, 12 asserts)
- Graph forward value **BIT-IDENTICAL** to a direct f64 evaluation of the functor.
- Graph gradient == the reverse tape (`<1e-11`) and == central FD (`<1e-6`).
- `optimize()` **strictly reduces** the node count (CSE + DCE) and is semantics-preserving; const-folds a `2*3`
  subgraph to `6.0` (no Mul node remains).
- `emit_cpp` produces a well-formed `crd_codegen_kernel(in,out,grad)` with per-output/gradient assignments.
- Full autodiff suite **2881 asserts / 113 cases** green.

## ★★ CRUSH — bit-identical codegen, 3.7× over the interpreted tape, 13.7× over JAX jit
**Config:** WSL2 i9-14900K, **1 thread `taskset -c 4`**, f64; Cerid g++ 13.3 `-O3 -march=native -ffp-contract=off`
(deterministic — see below); peer **JAX 0.10.2 `jax.jit(value_and_grad)`** (XLA CPU codegen, single-thread). Same
function `f(x)=x0² + Σ_i sin(x_i)·cos(x_{i+1}) + exp(0.1 x_i)`, n=32. Harnesses
`external/crd_v16h_codegen_bench.cpp` + `build/crd_v16h_codegen_bench.sh` + `scripts/v16h_codegen_peers.py`.

★ **The pipeline, live:** `281 forward nodes → 657 with-grad → 535 after CSE/DCE` (**−18.6%**), emitted to C++, compiled
by g++ (~1.9 s, once), dlopen'd.

★ **BIT-IDENTICAL codegen** — the compiled kernel's value AND gradient exactly equal the interpreter's
(`value=40.63346856439912`, `Sum_grad=28.99478179092168`). This required `-ffp-contract=off`: at `-O3 -march=native`
the straight-line codegen FUSES `mul+add` into an FMA the array-based interpreter doesn't ⇒ a 1-ULP drift; turning
contraction off restores bit-identicality AND is the correct deterministic-codegen policy (the determinism moat extends
to generated code).

★ **PARITY with JAX** — value `40.63346856439912` and `Sum_grad 28.99478179092168` match JAX to 14 digits.

★ **SPEED (value + full gradient, median):**

| | time | vs Cerid codegen |
|--|--:|--:|
| **Cerid CODEGEN** (dlopen'd kernel) | **316 ns** | 1× |
| Cerid interpreted graph (`eval`) | 607 ns | 1.9× |
| Cerid interpreted reverse TAPE | 1 169 ns | **3.7×** |
| JAX jit / XLA | 4 317 ns | **13.7×** |

The **3.7× over the interpreted tape** is the source-transform win (no per-node dispatch, no per-call tape rebuild, no
dynamic allocation — a compiled straight-line kernel). The **13.7× over JAX jit** is native C++ vs the XLA runtime.

## Portability + determinism (the moat vs the codegen peers)
- **Enzyme** = an LLVM plugin (source-transform AD tied to a specific LLVM); **JAX/XLA** = a heavyweight jit runtime.
  Cerid emits **plain C++** compiled by any C++ compiler — no plugin, no runtime — as hot-reload `.crds.cpp` cells
  (ADR-0081).
- **Deterministic generated code:** const-fold + emitted transcendentals both route through `crd::math`, and
  `-ffp-contract=off` bans FMA fusion ⇒ the codegen'd kernel is bit-identical to the interpreter AND run-to-run. XLA/
  Enzyme make no such guarantee.

## Verdict
- **Structural graph AD** (trace → symbolic reverse-AD → const-fold/CSE/DCE → interpret/codegen) — gated. ✓
- **Codegen BIT-IDENTICAL to the interpreter** (deterministic, `-ffp-contract=off`) — the dossier's codegen gate. ✓
- **CRUSH: 3.7× over the interpreted tape, 13.7× over JAX jit, value+grad parity to 14 digits, −18.6% nodes via CSE/DCE.** ✓
- Portable C++ (no LLVM plugin / XLA runtime), deterministic generated code. ✓
- **★ Follow-ons (honestly scoped):** op fusion beyond CSE, vector/SIMD codegen (the v16-e `vhvp` vector-op tape is the
  down-payment), and wiring the emitted cells into the live hot-reload loop (ADR-0081). 6-config DoD + {1..16} moat
  batched after v16.
