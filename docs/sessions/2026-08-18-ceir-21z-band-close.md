# CEIR-21z — the CEIR-21 (high-level tensor IR: `ceir.shape` / `ceir.tensor` / `ceir.layout`) band close (2026-08-18)

CEIR-21 gives the tensor/array/ML world the same authorable-IR treatment the render (18) / RT (19) / device-work (20)
bands gave graphics: **three value-op dialects over the CEIR-3d tensor TYPES** (Dim/Shape/Tensor, declared since 3d),
preserving tensor semantics ABOVE raw dispatch (§51/§70: "do not flatten a tensor graph to 100 dispatches before the
compiler decides whether the device runs it as one native graph program") — so **every op is Pure + typed NoSemantics +
carries NO kernel_ref / NO lowering hook**. crd-ceir gained ZERO gpu-context edges; the renderer is untouched.

## The band's sub-slices

- **21a** `ceir.shape` (§35) — the shape value-ops: `make` (dims→Shape) / `rank` / `extent` + `broadcast`/`reshape`
  (wrapping the 3d `shapes_broadcast`/`shapes_reshape` tri-state predicates) + `shape.assert` (the runtime-check DISCHARGE
  of the 3d "Unknown" deferral). `find_shape_misuse` (14 kinds: operand/result KIND + RESULT-IDENTITY [the 12a
  `underlying==operand` precedent] + the predicates WRAPPED). commit ⑲.
- **21b** `ceir.tensor` (§51 STRUCTURAL SIX) — elementwise/broadcast/reshape/transpose/reduce/matmul over the 3d Tensor
  type; the target shape RIDES the result type (validated, not operand-supplied); element = a 3a scalar kind. NEW
  `Context::shapes_broadcast_result` (the numpy-broadcast RESULT-shape producer the 3d row deferred). `find_tensor_misuse`
  (16 kinds: Tensor-kind + element-consistency + the 3d predicates + the NEW check classes — transpose PERMUTATION, reduce
  AXIS-bounds, matmul CONTRACTION + BATCH — + result-shape identity). commit ⑳.
- **21c** `ceir.layout` (§22) — ONE Pure op `layout.constrain(%tensor){kind,params}`, an OPTIONAL layout CONSTRAINT. ⛔
  MECHANISM (advisor-locked): a CONSTRAINT OP, NOT a Tensor-type member — the 21b Tensor type stays [element,shape]
  UNTOUCHED (a 3rd Layout member would make `tensor<f32,4x3>` ≠ `tensor<f32,4x3,row_major>` as TypeIds, reopening every
  21b identity check + rippling into type_is_canonical/decoder/grammar/fuzz). `find_layout_misuse` (8 kinds: Tensor-kind,
  passthrough identity, `kind` closed vocab, KIND-GATED params, arity-vs-rank). commit ㉑.
- **21z** — THIS: the composing gate + band close.

## MATRIX (band-scope × 4 configs — re-run at each sub-slice close + this gate, never inherited)

| Suite (tests) | win-debug | win-asan | linux-gcc-debug | linux-gcc-asan |
|---|---|---|---|---|
| **21a** `[shape]` (3 + 3 gen-smoke) | ✅ | ✅ | ✅ (gcc `-Werror=switch` 14-case) | ✅ |
| **21b** `[tensor]` (3 + 3 gen-smoke) | ✅ | ✅ | ✅ (16-case switch) | ✅ |
| **21c** `[layout]` (2 + 3 gen-smoke) | ✅ | ✅ | ✅ (8-case switch) | ✅ |
| **21z** `[band21]` composing gate (1) | ✅ | ✅ | ✅ | ✅ |

Plus per sub-slice: `crd-ceir-opgen-drift` / `-validator` + `crd-ceir-invariants` + LLVM-20 tidy, all GREEN. Each sub-slice
gate was RE-RUN after its advisor-at-the-gate fixes (never inherited — the GATE-reverify discipline). The 21z gate file
`test_band21_gate.cpp` is LLVM-20 tidy-clean (1 file).

⛔ **BAND-CLOSE FULL RE-RUN (the whole `ceir.21` band + all gen-smoke, ×4 configs — the 21a rows are INHERITED greens
since context.cpp changed at 21b, so the FULL band re-ran, never inherited):** win-debug **9 hand** (`[shape]` 3 +
`[tensor]` 3 + `[layout]` 2 + `[band21]` 1) + **51 gen-smoke**; win-asan 9 + 51; linux-gcc-debug **60** (9 + 51, REBUILT
74/74 — a genuine compile+run, not a stale binary); linux-gcc-asan 60 (REBUILT) + `crd-ceir-opgen-drift` (#665) +
`-validator` (#666) + `crd-ceir-invariants` (#1305) + gcc `-Werror=switch`, all GREEN. The reduce/matmul determinism doc
edit was opgen-REGEN'd → drift-clean two ways (`--check` byte-compare + ctest #665). ⛔ SELF-CAUGHT during this re-run: a
first Linux invocation FALSE-GREENED (exit 0, empty output) because the `wsl bash -lc` interop expanded the loop variable
`$p` to EMPTY (while `$HOME` expanded fine) — the loop ran twice over an empty preset path and no-op'd; re-run with LITERAL
preset paths (no loop var) → the genuine 74/74 rebuild + 60/60. See [[reference_wsl_lc_loop_var_expands_empty_use_literal_paths]].

## The COMPOSING GATE (21z, the 12z mold) — the band's key proof

`tests/ceir/test_band21_gate.cpp`: ONE C++-built module threads all THREE dialects —
`shape.make`/`shape.rank` (present + verified) alongside a chained tensor+layout pipeline:
**a rank-1 row tensor → `tensor.broadcast` UP → `layout.constrain{blocked}` → `tensor.elementwise` → `transpose` →
`matmul` → `reshape` → `reduce`-to-RANK-0** (§21 `!shape<>` — the case 21b deferred here; `drop_axis_shape` yields it and
`type_shape({})` interns it consistently, so the reduce result-identity matches). The band property PROVEN:

1. **The verifiers COMPOSE** — `find_shape_misuse` + `find_tensor_misuse` + `find_layout_misuse` + `find_structure_error`
   are ALL `None` on the ONE module (no walk falsely flags another dialect's ops).
2. **TEXT round-trip** — `print → parse → re-walk all-None` + `print(parsed) == print(original)` byte-exact (the 5d
   discipline).
3. **BINARY round-trip** — `serialize → deserialize → re-walk all-None`.

⭐ This is the FIRST time shape/tensor/layout ops cross the serializers — they round-trip byte-clean (the 3d types + the
21a-c ops all survive print/parse and serialize/deserialize).

## builder AUDIT — mandate #1 (author the asset; C++ only as cooker/verifier)

CEIR-21 is DECLARE-only: dialects + verifiers + tests, ZERO renderer changes. crd-ceir NEVER links gpu-context. The only
C++ is the `find_*_misuse` verifiers + `Context::shapes_broadcast_result` (a type-system predicate producer, the 3d
predicate family) — no render-technique builders, no kernel_ref, no lowering. `scene_renderer.cpp` UNTOUCHED.

## ROW-PER-CLAIM table (capability → gate → re-run)

| Claim | Gate (test #) | Re-run result |
|---|---|---|
| **§35 `ceir.shape`** — shape values/rank/extent + broadcast/reshape-compat (3d predicates WRAPPED) + `shape.assert` runtime discharge | `[shape]` #605 (well-formed) / #606 (13 malformed, exact kind) / **#607 (Unknown→ACCEPT tri-state, discharged by shape.assert)** | ✅ ×4 configs + gen-smoke #635–637 |
| **shape result-IDENTITY** (make members==dims, extent==member[axis], reshape==target, assert==lhs) | #606 (the 3 result-identity malformed) | ✅ ×4 (advisor-caught at 21a gate) |
| **§51 `ceir.tensor`** — the structural six (elementwise/broadcast/reshape/transpose/reduce/matmul) | `[tensor]` #608 (well-formed) / #609 (18 malformed) / #610 (Unknown-accept) | ✅ ×4 + gen-smoke |
| **`shapes_broadcast_result`** (the numpy-broadcast RESULT producer 3d deferred) | #608/#609 (elementwise/broadcast exactness) + #613 (band chain) | ✅ ×4 |
| **the NEW check classes** — transpose PERMUTATION, reduce AXIS-bounds, matmul CONTRACTION + BATCH | #609 (PermInvalid/AxisInvalid/ContractionMismatch/BatchMismatch) | ✅ ×4 (matmul BATCH false-green advisor-caught at 21b gate) |
| **§22 `ceir.layout`** — the optional constraint op, kind-gated params, arity-vs-rank | `[layout]` #611 (per kind + rank-3) / #612 (9 malformed) | ✅ ×4 (arity-0 leak advisor-caught at 21c gate) |
| **the COMPOSING property** — all 3 misuse walks + structure None on ONE module | #613 band gate | ✅ ×4 |
| **text + binary ROUND-TRIP** (the first serializer crossing for these ops) | #613 (print→parse re-walk + print==reprint; serialize→deserialize re-walk) | ✅ ×4 |
| **reduce-to-rank-0** `!shape<>` (the 21b-deferred case) | #613 (drop_axis→!shape<> + type_shape({}) interns) | ✅ ×4 |
| **mandate #1** — DECLARE-only, no gpu-context edge, no builder/kernel_ref | the builder audit above | ✅ (crd-ceir links no gpu-context; scene_renderer untouched) |

## ⛔ Dxxx DECLARED DIVERGENCE (spec-faithful, not silently dropped)

**§23 `tensor<Shape,Element,Layout>` (layout as a TYPE parameter):** CEIR-21c models layout as a CONSTRAINT OP, not a type
member — so the type-carried form is NAME-FORWARDED to the first consumer that needs layout IN the type IDENTITY (a §150
provider / a CEIR-22 interop boundary). Decided-with-reason (the identity-ripple + type-canonicalization cost), never a
silent downgrade of the §23 notation.

## DEFERRAL LEDGER (filed forward, each with a home)

| # | Deferred | Home |
|---|---|---|
| L1 | `ceir.linalg` (§52: GEMM/GEMV/solve/QR/LU/Cholesky/SVD/eigen identities) + FFT + solve/decomposition hooks | CEIR-22 (the GEMM→FFT→reduction charter) |
| L2 | the rest of §51's ops — gather/scatter, scans, sort/top-k, convolution | named-forward tensor-op rows |
| L3 | sparse/ragged forms + quantized element types (fp8/Q4/Q8) + packed-quantized/sparse LAYOUT kinds | CEIR-23 (`ceir.sparse` / `ceir.quant`) |
| L4 | element-TYPE PROMOTION (mixed-precision elementwise/matmul) — 21b requires SAME element; promotion rules | a future numeric-promotion slice |
| L5 | `tensor<S,E,L>` type-carried layout (the §23 Dxxx divergence above) | the first type-identity consumer (§150 provider / CEIR-22) |
| L6 | `shapes_broadcast_result` exactness DEFERRED above rank 16 (a bounded cap; no real tensor exceeds it) | a future unbounded-rank slice (if ever needed) |
| L7 | reduce `keepdim` (axis→1 instead of dropped) — 21b's reduce is rank-1 (axis removed) | a future reduce-variant slice |
| L8 | the canonicalizer must-not-fold `shape.assert` / `layout.constrain` passthroughs to identity | CEIR-6 canonicalization |
| L9 | reduce/matmul **DETERMINISM** — reductions + contractions are ORDER-SENSITIVE (float accumulation); the §27/§28 determinism class rides the EXECUTING tier, so the bit-exact contract can't be settled declare-side (declared in the reduce/matmul TOML docs, band-open instruction) | CEIR-22's ORACLE GATE (bit-exact GEMM→FFT→reduction) |
| L10 | `shape.assert`'s runtime EXECUTION — the Unknown-discharge op is declare-only; the 3d "Unknown → discharged by shape.assert" chain dead-ends until an executor runs the runtime check | the first runtime-shape-check executor (CEIR-22+ / the §121 REPL surface) |

## Verdict

✅✅✅ **CEIR-21 (high-level tensor IR: `ceir.shape` / `ceir.tensor` / `ceir.layout`) BAND CLOSED.** Three value-op
dialects over the 3d tensor types, DECLARE-only (Pure + typed NoSemantics, NO kernel_ref — §70 keeps the graph
whole for a native backend), and the composing gate PROVES they interoperate: on ONE module all three misuse walks +
structure are None (the verifiers compose), and the shape/tensor/layout ops survive BOTH serializers byte-clean.

- **21a/21b/21c** each gated **×4 configs** (win-debug/asan + linux-gcc-debug/asan) + opgen drift/validator + gcc
  `-Werror=switch` + tidy-20 + invariants, RE-RUN after each sub-slice's advisor-at-the-gate fixes.
- **21z composing gate**: ×4 configs GREEN — verifiers compose + text/binary round-trip byte-clean + the reduce-to-rank-0
  `!shape<>` case (21b-deferred) resolved.
- Advisor-at-the-gate at every sub-slice caught real defects (21a's result-identity gap; 21b's matmul BATCH false-green;
  21c's arity-0 param-skip leak) — all fixed and re-verified, never inherited.

The mandate held: crd-ceir gained ZERO gpu-context edges, no render-technique builders, no kernel_ref. The §23 layout-in-
type notation is a declared Dxxx divergence; **10 items** filed forward (L1–L10), each homed (→ CEIR-22 for
`ceir.linalg`/FFT + the reduce/matmul determinism ORACLE [L9] + `shape.assert` runtime EXECUTION [L10], CEIR-23 for
sparse/quant, CEIR-6 for canonicalization). Sub-slice + ledger detail is AUTHORITATIVE in this log + the tracker
CEIR-21 row. **NEXT BAND = CEIR-22 (CRD-Hesap integration — GEMM→FFT→reduction as ONE oracle-gated CEIR asset).**
